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
| `tcp_bridge` | Modbus TCP ↔ RTU re-framing, single client. Listener started/stopped by `net_runtime`; owns no Ethernet state. |
| `net_runtime` | Bounded `Ethernet.begin()`, link polling, TCP listener lifecycle, `NetState` |
| `gw_config` | Active/staged `GwConfig`, key table, parse + range + cross-field validation, `applyLive()` |
| `gw_store` | `TDBStore` on QSPI **partition 3**, CRC32 + magic blob, never formats or auto-writes |
| `gw_console` | The `$LGS` text protocol: line state machine, verbs, session arming |
| `gw_status` | Counters, RTT, LEDs, OTP MAC, reset reason, RTC-backed boot counter, safe mode |
| `main.cpp` | Setup ordering (see below) and the loop |

Function naming is module-prefixed: `tcpBridge_*`, `usbBridge_*`, `netRuntime_*`, `gwConfig_*`, `gwStore_*`, `gwConsole_*`, `gwStatus_*`, `rtu_*`.

### Setup ordering is the safety story

`setup()` is ordered so that **no stored value and no storage or network failure can cost the USB bridge**:

1. `Serial.begin` + relays/LEDs
2. `gwStatus_begin()` — reset reason, boot counter, **OTP MAC read before any QSPI mount**
3. button sample → recovery / erase
4. `gwStore_begin()` → `gwConfig_begin()` (failure degrades to defaults, never fatal)
5. **`usbBridge_begin()` + `gwConsole_begin()` — from here the bridge is live**
6. `netRuntime_begin()` — optional; worst case costs `net.link_timeout_ms` of boot time
7. `Watchdog.start(8000)`

Anything new that can block or fail goes **after** step 5.

## Runtime configuration

Edit over USB with `$LGS` lines (PuTTY at 115200 works; so does the Test Tool's Gateway tab):

```
$LGS PING | INFO | HELP | GET [key]
$LGS HELLO <who>              # arms a 120 s session — required before any write
$LGS SET key=value ...        # staged only
$LGS SAVE | DISCARD | DEFAULTS | REBOOT
```

Every command answers with exactly one terminal `#OK ...` or `#ERR ...` line, optionally preceded by `#DATA` lines. Errors are machine-readable (`err=range key=… allowed=…`).

Keys: `sys.name`, `rs485.baud|predelay_us|postdelay_us|t1_ms|t2_ms`, `usb.gap_ms|max_ms`, `net.enabled|dhcp|ip|mask|gw|dns|port|link_timeout_ms`, `net.mac` (read-only). `sys.log` is a verb-level volatile toggle, deliberately **not** in `GwConfig`.

## Hard-won gotchas — do not regress

- **`sys.log` must never be persisted.** A unit shipped with logging on would corrupt the SMT host's binary Modbus stream forever. It resets to 0 every boot by design.
- **The console magic is 5 bytes, `"$LGS "`.** A bare `$` is 0x24 = 36 = a real slave ID (row 3, col 6). The console only ever sees bytes `usb_bridge` already rejected as non-Modbus, and a valid RTU frame can never reach the parser — that ordering *is* the isolation, so never move the hook above the CRC gate.
- **Hand-typed console lines arrive one byte at a time.** The frame accumulator returns after ~10 ms of silence, so `gw_console` keeps its own line state across calls. Do not "simplify" it back into the accumulator.
- **`Ethernet.begin()` is bounded, not infinite** — but its default timeout is 60 s, which looks like a hang. The 7-arg overload takes an explicit timeout; `net_runtime` passes `net.link_timeout_ms` (1500 ms). `begin()` starts the interface non-blocking and merely *polls* for the link, which is exactly why a cable plugged in later is picked up by `netRuntime_update()` without a reboot.
- **`MbedServer::begin()` only allocates when its socket is null.** To move the listener to a new port you must `end()` first, or the re-bind silently fails.
- **Never use `kv_set`/`kv_get` on the Opta.** The default KVStore config claims the whole 16 MB QSPI over the MBR, WiFi firmware and OTA slots. Use `TDBStore` on `MBRBlockDevice(..., 3)` as `gw_store` does.
- **QSPI provisioning is a one-time manual step.** A board whose QSPI was never partitioned reports `cfg.store=unavailable`; fix it with Arduino's `QSPIFormat.ino` (partitions become WiFi 1 MB / OTA 5 MB / KVStore 1 MB / user data 7 MB). That sketch prints its banner before a late-attaching host sees it and then blocks silently, so send the first `Y` blind — and note `waitResponse()` accepts any stray y/n byte, so a command containing "N" answers "no" for you.
- **`_getSecureEthMac_()` returns the byte count 6, not a success flag**, and falls back to a raw flash pointer when the OTP magic misses. Validate the bytes; `gwStatus_macValid()` does. When it is false the network hands `begin()` a `nullptr` so mbed derives a per-board address instead of every gateway sharing one.
- In USB-bridge mode the COM port carries **binary RTU only**; one stray text byte corrupts the PC master's stream. That is why `LOG_SERIAL` is gated at runtime and off by default.
- **Broadcasts (address 0) must not wait for a reply.** OTA streams chunks back-to-back; a first-byte timeout per chunk lets the next frames merge in the USB buffer and fail CRC. Both bridges call `rtu_send()` for address 0.
- Module addressing: slave ID = `row*10 + col` on a **10×8** grid (11–18 … 101–108). Hardware-verified coils: 1021 (ON action), 1001 (OFF action).
- USB frame intake ends after `usb.gap_ms` of silence — if a PC tool produces split frames (CRC-drop symptoms), raise it rather than restructuring.
- The blue USER LED lit = the bridge is live; with logging off it is the only boot-success indicator. `LED_D1` = Ethernet link, `LED_D3` = fault/safe mode.
- Repo lives inside OneDrive. `.pio` is gitignored; if a build hits a file lock, just rerun (or pause sync).

## Conventions

- Refactors must be behavior-preserving unless asked otherwise; check RAM/Flash usage stays ~identical before/after as a transcription check.
- Commit messages in English, imperative mood, one topic per commit, every commit green.
- The user communicates in Thai — reply in Thai with English technical terms; README keeps an English body with a Thai summary section.
- Do not bump the version or rebuild release artifacts for small in-progress changes.
