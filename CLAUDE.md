# CLAUDE.md

Guidance for Claude Code when working in this repository.

## Project

Modbus gateway firmware for the **Arduino Opta** (STM32H747, arduino-mbed core) serving the LGS module grid over RS485. Two operating modes, selected at build time:

- **Modbus TCP gateway** — TCP clients on port 502 ↔ Modbus RTU on RS485
- **USB-RS485 bridge** — the PC talks raw Modbus RTU straight through the Opta's USB COM port (replaces a USB-RS485 dongle)

Cloned from [M-TRCH/LGS-Master](https://github.com/M-TRCH/LGS-Master) branch `Modbus-Gateway` @ `70eb03f`; history restarted July 2026. Remote: `M-TRCH/LGS-Gateway-Arduino-Opta`.

## Build / upload / test

PlatformIO project, single env `[env:opta]`. On this machine the CLI is
`C:\Users\mteer\.platformio\penv\Scripts\platformio.exe` (VS Code tasks Build / Upload / Monitor are preconfigured).

```
pio run                 # build
pio run -t upload       # flash over USB (DFU, auto 1200bps touch)
pio device monitor      # serial log @ 115200 (only useful when SERIAL_LOG_ENABLED=1)
```

Hardware-in-the-loop test (needs the Opta in USB-bridge mode + a powered RS485 slave):

```
C:/Users/mteer/.platformio/penv/Scripts/python.exe test/usb_bridge_test.py [--count N]
```

Uses PlatformIO's bundled Python (pyserial included); auto-detects the Opta COM port (VID:PID 2341:0164). There are no unit tests — verification is "build must pass" plus this script when hardware is present. Keep every commit building.

## Architecture

All first-party code lives in `src/` + `include/` (~600 lines):

| Module | Role |
|---|---|
| `config.h` | Every tunable: pins, network identity, bauds, timeouts, self-test grid, build-time switches |
| `modbus_rtu` | RTU transport: `crc16`, `verifyCRC`, `rtu_transact()` (pre-TX flush, timed RX, TX-echo strip), `writeCoil` (FC05), `rtu_setQuiet()` |
| `tcp_bridge` | Modbus TCP server (single client) ↔ RTU re-framing; MBAP validation, transaction-ID echo |
| `usb_bridge` | USB COM ↔ RS485 passthrough: gap-delimited frame intake (10 ms), CRC gate, transactional forward via `rtu_transact()` |
| `self_test` | Coil sweep + extended test over the module grid |
| `main.cpp` | Setup, build-time mode dispatch, (currently disabled) button handling |

Function naming is module-prefixed: `tcpBridge_*`, `usbBridge_*`, `selfTest_*`, `rtu_*`.

## Build-time switches (top of `config.h`)

| Define | Current | Meaning |
|---|---|---|
| `PANEL_BUTTONS_ENABLED` | 0 | External buttons compiled out (temporary — restore with 1) |
| `USB_BRIDGE_ON_BOOT` | 1 | 1 = boot as USB-RS485 bridge **with TCP/Ethernet fully compiled out**; 0 = TCP gateway |
| `SERIAL_LOG_ENABLED` | 0 | 1 restores all logs plus a 5 s `[SYS] alive` heartbeat and per-frame `usb_bridge` traces |

Logging goes through the `LOG_SERIAL` macro (config.h) so the whole stream compiles out. **New log lines must use `LOG_SERIAL`, never `Serial`** — the only direct `Serial` user is `usb_bridge`'s binary data path.

## Hard-won gotchas — do not regress

- **`Ethernet.begin()` on the arduino-mbed core blocks forever when no LAN cable is attached.** It stalls `setup()` before `loop()` ever runs while USB still enumerates normally, which looks like a totally dead-but-connected device. Never call it in a build that may run cable-less; USB-bridge builds compile it out entirely (`#if !USB_BRIDGE_ON_BOOT` in main.cpp).
- The extended self-test writes coil **1024 ON but 1004 OFF** — intentional asymmetry (commented in `self_test.cpp`), do not "fix".
- In USB-bridge mode the COM port carries **binary RTU only**; one stray text byte corrupts the PC master's stream. Boot-time prints are acceptable only because the host normally opens the port later.
- Module addressing: slave ID = `row*10 + col` on a 6×4 grid (IDs 11–64). RS485 bus is fixed 9600 8N1; the USB side is CDC, so PC baud settings are ignored. Hardware-verified coils on the modules: 1021 (ON action), 1001 (OFF action); self-tests use 1004/1024.
- USB frame intake ends after `USB_FRAME_GAP_MS` (10 ms) of silence — if a PC tool produces split frames (CRC-drop symptoms), raise it to 20 ms rather than restructuring.
- The blue USER LED (`LED_USER`) lit = USB-bridge mode active; it is the only boot-success indicator when logging is off.
- Repo lives inside OneDrive. `.pio` is gitignored; if a build hits a file lock, just rerun (or pause sync).

## Conventions

- Refactors must be behavior-preserving unless asked otherwise; check RAM/Flash usage stays ~identical before/after as a transcription check.
- Commit messages in English, imperative mood, one topic per commit, every commit green.
- The user communicates in Thai — reply in Thai with English technical terms; README keeps an English body with a Thai summary section.
