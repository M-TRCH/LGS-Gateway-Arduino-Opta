# CLAUDE.md

Guidance for Claude Code when working in this repository.

## Project

Modbus gateway firmware for the **Arduino Opta** (STM32H747, arduino-mbed core) serving the LGS module grid over RS485. Both front ends are always compiled and both can run:

- **Modbus TCP gateway** — TCP clients ↔ Modbus RTU on RS485. Used by LGS (medicine cabinets): a server pushes Modbus TCP over LAN.
- **USB-RS485 bridge** — the PC talks raw Modbus RTU straight through the Opta's USB COM port. Used by SMT (dispensing carts), where in production that COM port belongs to a *different* application, not the Test Tool.

Which one is live is **runtime configuration** (`net.enabled`), not a build switch. The USB bridge always runs.

Settings are edited over a `$LGS` ASCII console on the same USB port and persisted in QSPI. The companion GUI is [M-TRCH/LGS-Test-Tool](https://github.com/M-TRCH/LGS-Test-Tool) (Gateway tab).

Cloned from [M-TRCH/LGS-Master](https://github.com/M-TRCH/LGS-Master) branch `Modbus-Gateway` @ `70eb03f`; history restarted July 2026. Remote: `M-TRCH/LGS-Gateway-Arduino-Opta`.

## Build / upload / test

`test/tcp_console_test.py --mode matrix|fw-dry|two-client` exercises the unit-255 tunnel against the bench (needs pymodbus; run it with the Test Tool's venv).

PlatformIO project, single env `[env:opta]`. On this machine the CLI is
`C:\Users\mteer\.platformio\penv\Scripts\platformio.exe` (VS Code tasks Build / Upload / Monitor are preconfigured).

```
pio run                 # build
pio run -t upload       # flash over USB (DFU, auto 1200bps touch)
pio device monitor      # serial log @ 115200 (only useful after `SET sys.log=1`)
```

Hardware-in-the-loop tests (need powered RS485 slaves; the Test Tool venv has pyserial + pymodbus):

```
test/usb_bridge_test.py                     # coil toggle through the USB bridge
test/console_mux_test.py --mode mux         # console vs Modbus cross-talk (the decisive one)
test/console_mux_test.py --cmd INFO         # one-shot console command
test/tcp_bridge_test.py --mode compare      # TCP and USB must return identical data
test/tcp_bridge_test.py --mode soak         # both front ends at once
```

Anything holding the COM port (including the Test Tool) blocks both flashing and these scripts — stop it first, and put it back afterwards.

There are no unit tests. Verification is "build must pass" plus these scripts. Keep every commit building.

## Architecture

All first-party code lives in `src/` + `include/` (~2000 lines).

| Module | Role |
|---|---|
| `config.h` | Pins, LED map, and the **factory defaults** that seed `GwConfig`. Not the live values. |
| `modbus_rtu` | RTU transport: `crc16`, `verifyCRC`, `rtu_transact()` (pre-TX flush, timed RX, TX-echo strip), `rtu_send()` (broadcast, no reply wait), `rtu_setTimeouts()` |
| `usb_bridge` | USB COM ↔ RS485 passthrough: gap-delimited frame intake, CRC gate, transactional forward. Rejected bytes go to `gwConsole_feed()`. |
| `tcp_bridge` | Modbus TCP ↔ RTU re-framing, **two client slots** (`_client[2]`; a third connection is refused). lwIP is compiled for 4 sockets total (inside the prebuilt libmbed.a): listener + 2 clients + NTP's transient UDP = all four — never add a slot. Listener started/stopped by `net_runtime`; owns no Ethernet state. |
| `net_runtime` | Bounded `Ethernet.begin()`, link polling, TCP listener lifecycle, `NetState`; `startServer()` is the one "link just came up" funnel and kicks `ntp` |
| `ntp` | SNTP client: transient `EthernetUDP`, IP-only server (`net.ntp`), query on link-up + daily, 15-min retry, silent failure. UTC→wall via `time.tz_min` happens here ONLY |
| `event_log` | 16-byte-record ring on the RAW QSPI window **14–15.5 MB** (outside every QSPIFormat partition — partition 4 is LittleFS/PLC land, do not move it there). Non-fatal on storage errors; `$LGS LOG` |
| `gw_config` | Active/staged `GwConfig`, key table, parse + range + cross-field validation, `applyLive()` |
| `gw_store` | `TDBStore` on QSPI **partition 3**, CRC32 + magic blob, never formats or auto-writes |
| `gw_console` | The `$LGS` text protocol: line state machine, verbs, session arming. Output goes through a swappable emit sink (`gwConsole_execute`) so `gw_remote` can capture it |
| `gw_remote` | The gateway-self path on port 502: unit **255** / FC **0x41** carries the console (EXEC/READ paging over a 12 KB buffer) and the network firmware update (stage `UPDATE.BIN` on QSPI partition 2, CRC-verify, then the factory bootloader's RTC-magic apply via Arduino_Portenta_OTA). Gated by `net.console` |
| `gw_status` | Counters, RTT, LEDs, OTP MAC, reset reason (string + one-byte code), RTC-backed boot counter, safe mode |
| `panel` | Front-panel buttons (inputs 1-5), cabinet sweeps, and the four relay outputs — lamps and the shelf's power, all mapped from config |
| `sched` | Wall clock and the scheduled shelf reset. `sched_setTime(epoch, src)` is the single clock funnel — both the console's TIME and NTP hand it WALL time |
| `main.cpp` | Setup ordering (see below) and the loop |

Function naming is module-prefixed: `tcpBridge_*`, `usbBridge_*`, `netRuntime_*`, `ntp_*`, `eventLog_*`, `gwConfig_*`, `gwStore_*`, `gwConsole_*`, `gwStatus_*`, `panel_*`, `sched_*`, `rtu_*`.

### Setup ordering is the safety story

`setup()` is ordered so that **no stored value and no storage or network failure can cost the USB bridge**:

1. `Serial.begin` + relays/LEDs
2. `gwStatus_begin()` — reset reason, boot counter, **OTP MAC read before any QSPI mount**
3. button sample → recovery / erase
4. `gwStore_begin()` → `gwConfig_begin()` (failure degrades to defaults, never fatal)
5. `gwStatus_watchdogBegin(sys.wdt_ms)` — **everything below is watchdog-covered**
6. `eventLog_begin()` — the 1.5 MB boot scan runs under the fresh watchdog; a QSPI fault turns the log off (`log.state=off_error`), never the boot
7. **`usbBridge_begin()` + `gwConsole_begin()` — from here the bridge is live**
8. `netRuntime_begin()` — optional; worst case costs `net.link_timeout_ms` of boot time, and the watchdog is kicked immediately before it

Anything new that can block or fail goes **after** step 7.

## Runtime configuration

Edit over USB with `$LGS` lines (PuTTY at 115200 works; so does the Test Tool's Gateway tab) — or over TCP: unit id 255 / FC 0x41 on port 502 tunnels the same verbs, same session, same staged-edit set (see `gw_remote.h` for the byte layout). `net.console=0` closes the TCP path; only USB can reopen it.

```
$LGS PING | INFO | HELP | GET [key]
$LGS LAMP <1-4|off> [ms]      # drive one relay output, for wiring checks
$LGS TIME [epoch]             # read or set the wall clock (LOCAL seconds)
$LGS LOG [n]                  # newest n event-log records (default 20, cap 100)
$LGS HELLO <who>              # arms a 120 s session — required before any write
$LGS SET key=value ...        # staged only
$LGS SAVE | DISCARD | DEFAULTS | REBOOT
```

`LAMP`, `TIME` and `LOG` are deliberately **not** session-gated: none outlives its own timeout (LOG is read-only), and a clock that needs a session to correct is a clock that spends its life wrong. LOG kicks the watchdog per emitted line — the USB CDC blocks while a connected host is not reading, and a 100-line dump into a stalled terminal must not reset the gateway.

Every command answers with exactly one terminal `#OK ...` or `#ERR ...` line, optionally preceded by `#DATA` lines. Errors are machine-readable (`err=range key=… allowed=…`).

Keys: `sys.name`, `sys.wdt_ms` (reboot-only), `rs485.*`, `usb.*`, `net.*` (`net.mac` read-only; `net.ntp` = NTP server **IP**, 0.0.0.0 off; `net.ntp_port`; `net.console` = the TCP tunnel gate, default on), `time.tz_min` (the one SIGNED key — `KIND_I16` is hard-wired to it the way `KIND_STR` is to `sys.name`; default 420 = Thailand), `bus.hub_map|hub_retry|hub_gap_ms|hub_settle_ms|hub_budget_ms`, `panel.enabled|cabinet|shape|btn1..btn5|step_ms|reset_ms|lamps|lamp_hold_ms|lamp_dwell_ms|lamp_dead|out1..out4`, `sched.reset_enabled|reset_hhmm|reset_hhmm2|reset_hhmm3|reset_hhmm4|reset_slots|reset_days`. `sys.log` is a verb-level volatile toggle, deliberately **not** in `GwConfig`. `panel.shape` (slots per row, `8,8,4` style) overrides `panel.cabinet`'s preset for the button sweeps when non-zero — the escape hatch for a cabinet that is not a 40/64/80; both row-list keys share one parser (`parseRowList`/`formatRowList`). `panel.preset` (1-8) picks which module preset the sweeps fire (coils `1010+p`/`1030+p`) — brightness and colour live in that preset's per-module config, so the panel's look is tuned on the modules, not in the gateway. `panel.bright` (0-100, 0 = off) is the TEST brightness: written per slot to the module's volatile reg 190 before lighting, never persisted module-side — deliberately, so a bench test cannot rewrite a site's configured look.

The key table is **append-only**: `valueOf()`/`storeValue()` switch on the index, so inserting a key silently rewrites the meaning of every key after it.

## Hard-won gotchas — do not regress

- **`sys.log` must never be persisted.** A unit shipped with logging on would corrupt the SMT host's binary Modbus stream forever. It resets to 0 every boot by design.
- **The console magic is 5 bytes, `"$LGS "`.** A bare `$` is 0x24 = 36 = a real slave ID (row 3, col 6). The console only ever sees bytes `usb_bridge` already rejected as non-Modbus, and a valid RTU frame can never reach the parser — that ordering *is* the isolation, so never move the hook above the CRC gate.
- **Hand-typed console lines arrive one byte at a time.** The frame accumulator returns after ~10 ms of silence, so `gw_console` keeps its own line state across calls. Do not "simplify" it back into the accumulator.
- **`Ethernet.begin()` is bounded, not infinite** — but its default timeout is 60 s, which looks like a hang. The 7-arg overload takes an explicit timeout; `net_runtime` passes `net.link_timeout_ms` (1500 ms). `begin()` starts the interface non-blocking and merely *polls* for the link, which is exactly why a cable plugged in later is picked up by `netRuntime_update()` without a reboot.
- **`MbedServer::begin()` only allocates when its socket is null.** To move the listener to a new port you must `end()` first, or the re-bind silently fails.
- **The stored blob's `schema` must be checked, and bumped when a field's MEANING changes** even if the struct size does not. It carried a schema field for months that nothing verified, and the day three bytes went from "which output is this colour on" to "what does this output follow", the old values were read straight back in and lit the wrong lamp. Size alone does not catch that.
- **A schema or struct-size bump wipes the unit's settings** — the load reports `CORRUPT` and the board comes up on factory defaults, which means Ethernet **off** and no hub map. There is no migration path. Read the settings out (`$LGS GET`) before flashing a firmware whose `GwConfig` changed, and re-provision after: on the bench that is `net.enabled=1`, `net.ip`, `bus.hub_map`, `panel.enabled=1`. The Test Tool's `data/config.json` keeps the hub map and the address, which is the only reason the last one was recoverable.
- **The watchdog must outlast the boot-time wait for an Ethernet link.** It now starts partway through `setup()` so it covers the network call, which means a period shorter than `net.link_timeout_ms` (+ the DHCP allowance) resets the board mid-boot, forever. `gwConfig_validateStaged()` refuses that combination; keep the check if either value moves. `NET_DHCP_RESPONSE_MS` lives in `config.h` for exactly this reason — a second copy in `net_runtime.cpp` would let the two drift.
- **Anything that drives a relay must ask whether it is the shelf's power first.** The lamp test drives one output and clears the others; the lamps-off switch clears them all; the dwell rate-limits them. All three would cut the cabinet's power if they treated `SRC_SHELF` as a lamp, so each one skips it explicitly.
- **The panel's sweeps must step from `loop()`, never block.** Eighty slots is several seconds of Modbus and the USB bridge, the TCP bridge and the watchdog all have to keep running through it.
- **Nothing may be scheduled while the clock is unset.** The Opta's RTC has no battery, so a gateway that lost power believes it is January 1970 — and would fire its 03:00 reset the moment that fiction reached 03:00. `sched_update()` returns early unless `sched_setTime()` has been called since boot.
- **The clock keeps wall time, not UTC**, and there is no timezone anywhere in the firmware. A tool setting it must send local seconds or every schedule is silently out by the offset.
- **`INFO` values must not contain spaces.** The console's `key=value` lines are split on whitespace, so `time.now` is ISO with a `T`; a space truncated the value at the reader. `sched.reset` broke this rule unnoticed for two versions — `"03:00 daily"` arrived as `03:00` — which is why it now reads `03:00,15:00@Mon,Wed` and says `waiting_for_clock` rather than "waiting for the time".
- **The RS485 hub needs ~2 s of silence to change channel** (measured; see `modbus_rtu.cpp`). Repairing by retrying inside one transaction cannot work — the fix is to hold the next request in silence until the channel opens, and `bus.hub_budget_ms` must stay under the master's timeout or the bridge desynchronises.
- **Never use `kv_set`/`kv_get` on the Opta.** The default KVStore config claims the whole 16 MB QSPI over the MBR, WiFi firmware and OTA slots. Use `TDBStore` on `MBRBlockDevice(..., 3)` as `gw_store` does.
- **QSPI provisioning is a one-time manual step.** A board whose QSPI was never partitioned reports `cfg.store=unavailable`; fix it with Arduino's `QSPIFormat.ino` (partitions become WiFi 1 MB / OTA 5 MB / KVStore 1 MB / user data 7 MB). That sketch prints its banner before a late-attaching host sees it and then blocks silently, so send the first `Y` blind — and note `waitResponse()` accepts any stray y/n byte, so a command containing "N" answers "no" for you.
- **`_getSecureEthMac_()` returns the byte count 6, not a success flag**, and falls back to a raw flash pointer when the OTP magic misses. Validate the bytes; `gwStatus_macValid()` does. When it is false the network hands `begin()` a `nullptr` so mbed derives a per-board address instead of every gateway sharing one.
- In USB-bridge mode the COM port carries **binary RTU only**; one stray text byte corrupts the PC master's stream. That is why `LOG_SERIAL` is gated at runtime and off by default.
- **Broadcasts (address 0) must not wait for a reply.** OTA streams chunks back-to-back; a first-byte timeout per chunk lets the next frames merge in the USB buffer and fail CRC. Both bridges call `rtu_send()` for address 0.
- Module addressing: slave ID = `row*10 + col` on a **10×8** grid (11–18 … 101–108). LGS-64 is not a rectangle: rows 1–3 and 8–10 are eight wide, rows 4–7 four wide.
- Module coils are combinations, not steps: 1001 ring, 1011 ring + number, 1021 ring + latch, 1031 all three. The latch coils self-clear and mirror their state twin, so 1021 clears through 1001 and **1031 clears through 1011** — clearing 1031 through 1001 puts the ring out and leaves the display lit.
- USB frame intake ends after `usb.gap_ms` of silence — if a PC tool produces split frames (CRC-drop symptoms), raise it rather than restructuring.
- The blue USER LED lit = the bridge is live; with logging off it is the only boot-success indicator. `LED_D1` = Ethernet link, `LED_D3` = fault/safe mode.
- Repo lives inside OneDrive. `.pio` is gitignored; if a build hits a file lock, just rerun (or pause sync).
- **The network firmware update applies ONLY in FW_APPLY, only after a CRC pass over the file read back from QSPI.** The RTC apply-magic (DR0=0x07AA + DR1..DR3, written by Arduino_Portenta_OTA's `update()`) is the bootloader's trigger; writing it anywhere else turns a half-uploaded file into a boot image. Never call the library's `begin()` — it also mounts the WiFi partition to check a TLS certificate that only its cloud-download path needs.
- **Relay outputs are re-asserted every `PANEL_REASSERT_MS`, never only on change.** They used to be written once and then only when the commanded state changed, so a pin that lost its level any other way would have stayed wrong forever with the firmware sure it was right. The read-back can only see a corrupted GPIO latch, NOT a relay whose coil supply sagged — that one shows up as `bus_quiet` instead. The drift report is one event per episode, not one per tick: a board that ever reads its outputs back differently from how it drives them must not turn the log into a stream.
- **`bus_quiet` is the site's black box.** Six consecutive RTU timeouts = every slave silent at once, which is what a cabinet losing power looks like from here; the event log then carries the timestamp with no PC involved. One dead module can never trip it — the poll moves on to the next address.
- **Stretch the IWDG before the post-APPLY reset** (`gwRemote_update()`): the IWDG cannot be stopped and may ride through a software reset, and the bootloader's ~230 KB copy can outlast an 8 s period. PR/RLR are writable while running; the next boot restores the configured period.

## Conventions

- Refactors must be behavior-preserving unless asked otherwise; check RAM/Flash usage stays ~identical before/after as a transcription check.
- Commit messages in English, imperative mood, one topic per commit, every commit green.
- The user communicates in Thai — reply in Thai with English technical terms; README keeps an English body with a Thai summary section.
- Do not bump the version or rebuild release artifacts for small in-progress changes.
