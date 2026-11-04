# Design note: the Opta as a depot for module firmware (DRAFT, nothing built)

Status: proposal, written 2026-11-04 from a conversation with the maintainer. No code yet.

## The problem (site constraints)

- The Test Tool is installed on the hospital server, but it can be reached only from a PC or handheld inside each
  medication room, and getting a new firmware file onto that server remotely is hard. The installed Tool carries no
  firmware bundle.
- A file can reach the system one way: a laptop plugged **directly** into an Opta (LAN cable, point to point, no
  hospital network; USB-C is not needed). About one minute per Opta is acceptable.
- A laptop cannot stay plugged into one Opta while the modules are updated: a room holds many cabinets.

## Chosen shape: the Opta is a network "USB stick", the Tool stays the orchestrator

1. **Deposit** (a visit, ~1 min per Opta): the laptop's Tool (the copy that has the images) writes the module OTA image
   into the Opta over the existing unit 255 / FC 0x41 tunnel. The Opta keeps it on QSPI and CRC-checks it.
2. **Update** (later, from the room PC, no laptop): the Tool on the hospital server **reads the image back out of one
   Opta** over the same tunnel, holds it in memory, and runs the existing, bench-proven module OTA (`ota.py` /
   `ota_fleet.py`) against every cabinet in parallel through each gateway's TCP bridge, exactly as the Firmware page
   already does with a bundled image.

Why this and not "port the OTA runner into the gateway": `ota.py` is 545 lines of protocol carrying hard-won field
lessons (hub channels, bitmap repair, keepalive, slow retry, pacing). Porting it means re-learning them in firmware,
stepping it from `loop()` without blocking, and giving it exclusive ownership of the RS485 bus while the USB bridge and
TCP bridge keep running. Keeping the orchestration in Python removes all of that: the firmware only has to store, list,
read back and delete files. Revisit only if the update must run with the server off.

## Firmware work (gateway, new minor version)

New subfunctions next to FW_* in `gw_remote` (same 207-byte PDU limit, same `net.console` gate):

| sub | name | request | response |
|---|---|---|---|
| 0x20 | DEPOT_BEGIN | slot name (ASCII, 1..24, `[A-Za-z0-9._-]`) · size u32 · crc32 u32 | status |
| 0x21 | DEPOT_DATA | offset u32 · data[1..240] | status · received u32 |
| 0x22 | DEPOT_LIST | index u8 | count u8 · name · size u32 · crc32 u32 · state |
| 0x23 | DEPOT_READ | name · offset u32 | len u8 · data[<=200] (the whole file in ~310 requests, a few seconds) |
| 0x24 | DEPOT_DELETE | name | status |

- Storage: files `/fs/DEPOT/<name>` on the same FAT partition 2 that holds `UPDATE.BIN` (5 MB, ~4.7 MB free, images are
  ~61 KB). Cap at a few images; size limit 128 KB per file (the module slot is 61,440 B).
- Writes need the armed `HELLO` session like `FW_BEGIN`; listing and reading do not.
- CRC-32 is checked on the flash after the last chunk (as APPLY does), and again by the Tool after it reads the image
  back. A depot file is never executed by the gateway: it is bytes in transit.
- Logs `GW_EV_FW` with new aux codes (deposit begun / done / deleted); no new record layout.
- No new `GwConfig` key, so no `GW_BLOB_SCHEMA` bump and no wiped settings.
- Setup ordering is untouched (the depot lives in the TCP/console path, after step 7).

### Risks to settle on the bench before relying on it

1. **Does the bootloader's APPLY leave other files in partition 2 alone?** The partition is FAT; `fwMount()` already
   formats it if it cannot mount. A format after a power cut, or a bootloader that cleans the partition, would erase the
   depot. Test: deposit an image, run a gateway network update, list again. If it does not survive, the depot must
   move (partition 4 is the Arduino PLC area, so avoid it) or the Tool must re-deposit after every gateway update.
2. A power cut mid-write leaves a partial file: the CRC in DEPOT_LIST / the Tool's check catches it, and DEPOT_BEGIN over
   the same name starts clean.
3. TCP clients: the depot read runs while the hospital's own server may be polling the same Opta. It uses the second
   client slot like the console tunnel does; never add a slot (lwIP is full).

## Tool work

- Firmware page, deposit mode (laptop): "Put this image on the gateway" (pick image, pick gateway, shows size, CRC and the
  list the gateway now holds). Reuses the connection logic of the gateway network update.
- Firmware page, update mode (server): the image source gains "from a gateway": choose the gateway that holds it, the Tool
  pulls it, verifies the CRC and then continues down the existing module OTA path. A table shows which gateways hold which
  image, so a missed visit is visible before an update, not during.
- Nothing about the existing bundled-image path changes.

## Not decided / not covered

- **Gateway firmware itself** has the same distribution problem (the Tool needs the Opta image to update an Opta). The same
  depot could hold it, with the Tool pushing it to the other gateways. Left out until asked.
- This design assumes the server's Test Tool can reach every Opta on port 502 (it must, to poll and to run OTA today).
  If a room's server cannot reach some Opta, that Opta needs the laptop for its update as well.
- The module OTA occupies a cabinet's RS485 bus for minutes per hub channel; the hospital's own server traffic to that
  cabinet should be paused. That is true of the current Firmware page too and is unchanged here.
- Every Opta needs the new gateway firmware before it can hold a depot: one laptop visit each with the existing
  network update (about 47 s on the bench).

## Suggested order

1. Gateway: DEPOT_* subfunctions, bench-test against one Opta with `test/` scripts, run risk 1.
2. Tool: deposit and read-back on the Firmware page, selftest against a fake gateway.
3. One real module OTA from a depot image on the bench cabinet (Std-11 is the usual test cabinet).
4. Roll the new gateway firmware out, then deposit.
