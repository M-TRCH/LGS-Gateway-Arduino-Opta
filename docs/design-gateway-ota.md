# Design note: the gateway runs the module OTA itself, in the background (DRAFT, written 2026-11-04)

Builds on `design-module-depot.md` (the depot is the image source). The Test Tool only **starts** it and **reads** progress and
results; the gateway drives the bus, writes the results down, and survives the Tool disappearing.

## Requirements (from the maintainer)

1. **No downtime and no interference with the bus.** A dispensing command (any request on the TCP or USB bridge, a panel
   sweep) always wins. The update may pause and resume by itself.
2. The gateway keeps the record: which image, when, and for every module the firmware before and after and what happened.
3. The Tool is a trigger and a reader, nothing else. It may be switched off the moment the update has started.

## What the module side allows (read from LGS-Standard-Module `ota_control.cpp`)

- OTA is extra holding registers 282-399 and coils 505-508 on the module's ordinary Modbus server. Lights, display and latch
  coils are **not** gated by the OTA state: a module that is receiving an image still serves picks. (The OLED shows the
  update face while receiving; whether a pick's display write replaces it was not checked.)
- ENTER (coil 505) erases the 62 KB staging area, ~0.7 s during which that module does not answer; it fails with LATCH_BUSY if
  the latch FSM is not idle. A **unicast** FC16 metadata write + unicast coil 505 puts only chosen modules into the session; the
  broadcast chunk frames are ignored by every module whose state is not RECEIVING.
- RECEIVING times out after **90 s** without a chunk commit (30 s before v3.5.2) and the bitmap is wiped; VERIFIED has **no**
  timeout and waits for APPLY. So: staging can be paused for a while; a longer pause means re-entering that group.
- APPLY (coil 507) resets the module: the only real unavailability, ~5 s per module. It can be done module by module, later.

## Behaviour

**The bus rule.** `modbus_rtu` records when anything other than the OTA runner last used the bus (`rtu_msSinceForeign`) and
when each module was last *written* by a foreign caller (`rtu_msSinceWrite(id)`). The runner is stepped from `loop()` after
the bridges, one RS485 transaction (or one timer check) per call, so it never interrupts an exchange and a request that
arrives while it transmits waits at most one frame (a 128-byte chunk is ~170 ms at 9600 baud).
- It transmits only when the bus has been quiet for `idle` seconds (default 5). Any foreign traffic resets that clock.
- While paused it re-sends chunk 0 (a duplicate, harmless) at most every 8 s, **only** inside an idle window, to keep modules
  in the session.
- After a hub-channel crossing (a pick on another channel) it waits for the hub's settle time and re-parks on its own channel
  with one unicast read before the next chunk; the first frame after a crossing is lost by design and the bitmap repair
  re-sends it.

**Staging in small groups.** Per hub channel, at most `group` modules (default 8) at a time, so most of a cabinet is never
touched: unicast probe (firmware version), broadcast metadata, unicast ENTER to the group only, stream with the proven
pacing (a pause every 8 chunks), bitmap + state repair, finalize, read state. A module that left the session is retried once
with the slow pacing; a group whose sessions keep dying while the bus is busy is restarted from ENTER (at most 6 times), then
its modules are recorded as `gave_up` and the run goes on.

**Applying later, when it is safe.** Verified modules wait (no timeout). A module is applied only when the bus has been quiet
for `apply_idle` (default 20 s) **and** nobody wrote to that module for `apply_quiet` (default 300 s: a lit slot waiting for a
confirm must not be rebooted). A pick that arrives while the module reboots is held up to the existing hub budget; after that
it gets the normal timeout. This is the one window where a module cannot serve a request; it is ~5 s, per module, only when
that module was idle.

**Not promised:** literally zero impact. A pick can land during a module's 0.7 s staging erase or its 5 s reboot, and the
OLED shows the update face while a module is receiving. The design makes both rare and short, never blocks a request, and
never reboots a module that was recently written.

## Records

- **Result file** on the OTA FAT partition (`/fs/OTARES0.BIN`, `/fs/OTARES1.BIN`, alternating, so the two latest runs survive):
  header (image name, size, CRC, start/finish wall time, state) + a row per module (id, state, error, firmware before/after) +
  CRC-32. Rewritten at milestones (group staged, module applied, finish), never per chunk. A boot that finds a "running"
  record marks it `interrupted`.
  (The raw QSPI strip above 15.5 MB is the WiFi firmware map, not free space; FAT also keeps its own wear levelling.)
- **Event log** (`GW_EV_OTA`, 16-byte records unchanged): start, group staged, paused too long and re-entered, module applied,
  finished / aborted / interrupted. The Tool's collector picks these up for free.

## Console verbs (USB or the unit 255 tunnel; START and ABORT need HELLO)

```
$LGS OTA START slot=<0-3> [ids=11-18,21 | all] [idle=5] [group=8] [apply_idle=20] [apply_quiet=300]
$LGS OTA STATUS      state, group/channel, phase, chunk progress, counts staged / applied / failed, paused-for seconds
$LGS OTA RESULT [n]  the stored run n (0 = latest, 1 = the one before), one #DATA line per module
$LGS OTA ABORT       sends the module abort coil to the group in session, then stops; verified modules keep waiting
```
`ids` defaults to the cabinet's own slot list (`panel.cabinet` / `panel.shape`). START is refused when: another OTA is running, a
gateway firmware stage is receiving, the slot is not a complete image or is larger than the module slot (61,440 B), the clock
is unset (the record needs wall time), or the USB bridge has seen traffic in the last 60 s (an SMT PC owns that port).

## Out of scope / later

- Resuming a staging session across a gateway reboot (the record says `interrupted`; the operator starts again).
- Scheduling the update for a time window (START takes the quiet-bus rule instead).
- Tool UI: a card on the Firmware page to start on chosen cabinets, a status table polled from `OTA STATUS`, and a results
  view from `OTA RESULT`. Written after the firmware works on the bench.

## Test plan (Chest-Std-11, the bench cabinet)

1. Quiet run: one group, one run, results file and event log match what happened.
2. **Interference run**: a script drives picks (coils on and off) through the TCP bridge during staging; every pick answered,
   the worst added latency measured and compared with an idle baseline.
3. Pause beyond the session timeout (a busy spell > 90 s): the group is re-entered and finishes.
4. Abort mid-stream; power-cycle the gateway mid-run (record says interrupted, modules recover on their own timeout).
5. Apply deferral: write to a module, confirm it is skipped until `apply_quiet` has passed.
