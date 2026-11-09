# Design note: service Wi-Fi access point on the Opta WiFi (merged plan, 2026-11-05)

Supersedes the 2026-11-05 first draft and merges the earlier plan of 2026-10-14
(`C:\Users\mteer\.claude\plans\service-ap-config-whimsical-dijkstra.md`, second half, which was on hold).
Target firmware: **1.15.0**. Built on branch `service-ap` from `main` (v1.14.0).

## Goal

A technician at a cabinet, with only a phone, turns on a Wi-Fi access point on the gateway, opens a page, sees the
gateway's state and log, walks the modules, and sets the network. A laptop on the same AP can run the Test Tool as usual.
**The site's Modbus TCP server keeps working the whole time.** Off by default, never on at boot, off again by itself.

## Facts the design rests on (all checked in the installed core, framework-arduino-mbed 4.6.0, variant OPTA)

| Fact | Source |
|---|---|
| Only the **Opta WiFi (AFX00002)** has the radio (Opta RS485 = AFX00001, Opta Lite = AFX00003). The blue LED above the user button exists only on the WiFi model. | official datasheet, table 1, 5.7, 5.11 |
| The OTP board info (`OptaBoardInfo`, `boardInfo()`, magic 0xB5) carries a `wifi` bit. | `variants/OPTA/opta_info.h`, `variant.cpp` |
| `WiFi.beginAP(ssid, pass, ch)` = `WhdSoftAPInterface::start(..., WPA2, ch, dhcp server, coexistence)`. | `libraries/WiFi/src/WiFi.cpp` |
| The DHCP server owns **one UDP socket + one thread** while the AP is up. | `CyDhcpServer.h` |
| lwIP: **4 sockets in total** (TCP 4, UDP 4), 16 KB heap, compiled into `libmbed.a`. | `variants/OPTA/mbed_config.h` |
| The radio firmware is read from the file **`/wlan/4343WA1.BIN`** on QSPI partition 1 (FAT, label `wlan`), 421,098 B. The image is in `libmbed.a` (`4343WA1_bin.o`, `wifi_firmware_image_data`). The CLM blob is linked in. | `wiced_filesystem.o`, `4343WA1_bin.o` |
| If that file is missing, the core's `whd_firmware_check_hook()` (strong, in WiFi.cpp) prints and **spins forever** (`while(1)`, after `while(!Serial)`). | `WiFi.cpp` 362-401 |
| Arduino's `WiFiFirmwareUpdater` erases QSPI **14-16 MB** for its memory-mapped copy: that is this gateway's **event log**. It must never be run on a deployed unit. | `WiFiFirmwareUpdater.ino` |
| QSPIFormat formatted partition 1 as FAT on every bench unit; the firmware file was not restored (answered "n"). | earlier plan |
| `MbedServer::begin()` binds port 502 with no address: the one listener should accept on the AP too. | `SocketWrapper/src/MbedServer.cpp` |

## The socket budget, and why the page lives on port 502

| Socket | Today | AP up |
|---|---|---|
| port-502 listener | 1 | 1, shared by LAN and AP |
| client slots | 2 | 2, shared by LAN and AP |
| NTP query (transient) | 1 when it runs | 0: **paused while the AP is up** |
| AP DHCP server | - | 1 |

A separate HTTP listener does not fit, and the earlier plan's answer (stop the Modbus bridge) costs the site its server.
So **the page is served on port 502 itself**: a Modbus TCP frame has protocol id 0 in bytes 2-3, an HTTP request starts
with `GET `, so the first bytes of a request tell the two apart without doubt. The phone opens `http://192.168.77.1:502/`
(502 is not on any browser's blocked-port list). HTTP is answered only for clients on the AP subnet; on the LAN a
`GET` gets no answer and the connection is closed (since 1.15 any client whose first bytes are not Modbus TCP is closed:
draining and keeping it let a browser tab on the LAN hold a client slot). Every response is `Connection: close` and the page is one file with no favicon, so a
phone holds a slot for milliseconds.

## Behaviour

### Model and radio firmware
- `INFO` gains `hw.otp=0|1 hw.wifi=0|1 hw.rs485=0|1 hw.pid=0x....` from the OTP, and `wifi.fw=ok|missing|bad_size|nofs|n/a`
  from a read-only check of partition 1 (mounted under the name `wfw`, never `wlan`, then unmounted).
- **Network install of the radio firmware** (no site visit, no USB): unit 255 / FC 0x41 subfunctions
  `0x30 WIFI_FW_BEGIN size u32 · crc32 u32`, `0x31 WIFI_FW_DATA offset u32 · data[1..240]`, `0x32 WIFI_FW_STATUS`. HELLO
  required; refused while the AP is up, a module run is active or a gateway image is arriving. Written to
  `4343WA1.TMP`, CRC-checked on read-back, then renamed to `4343WA1.BIN`. If partition 1 does not mount, it is formatted
  (it holds only the radio files). The raw QSPI strip 14-16 MB is never touched. Test script:
  `test/wifi_fw_install.py` (extracts the image from `libmbed.a` with the toolchain's `objcopy`).

### Turning it on and off
- `$LGS AP ON [min=N] [bridge=0|1]`, `$LGS AP OFF`, `$LGS AP` (status), `$LGS AP PASS <8-31 chars>`, `$LGS AP SSID <1-31 chars>|default` (aux key `apn`, no spaces; default LGS-<sys.name>). ON, OFF and PASS need
  HELLO. Volatile: nothing about the AP survives a reboot except the password.
- **Front panel**: a new panel action `service_ap` (code 7) toggles it. (The earlier plan's BTN_USER hold needs the
  boot-button fix below first; that is a separate change.)
- Refused unless `hw.wifi=1`, `wifi.fw=ok` and a password is set. `beginAP` is never called at boot.
- Start runs from `loop()`, not from the console handler: kick, **stretch the IWDG to ~32.7 s**, pause NTP,
  `WiFi.config(192.168.77.1/24)`, `WiFi.beginAP("LGS-<sys.name>", pass, 6)`, restore the watchdog to `sys.wdt_ms`, kick.
  A hang inside the driver is therefore a reset after 32 s, never a dead unit; and since the AP is never on at boot it
  cannot loop.
- Off by: `AP OFF`, the panel action again, `min` minutes (default 30, at most 480) with no AP client, a reboot.
- LED_D1 blinks at 1 Hz while the AP is up (every other LED is taken).

### Credentials
- WPA2 only. The password is a **second key (`ap`) in the same TDBStore** on partition 3, beside the config blob:
  `GwConfig` and its schema do not change, so no unit loses its settings. `AP PASS` stores it; nothing ever prints it.

### What an AP client may do
- **Console page and the unit 255 tunnel**: always (the console's own HELLO gate still applies to writes).
- **Modbus bridge to the modules** (any other unit): refused with exception 0x0A for AP clients unless `AP ON bridge=1`.
- Told apart by the client's address (192.168.77.0/24). `net.ip` inside that subnet is refused by the validator.

### The page (served on port 502 to AP clients)
- `GET /` one inline HTML file (< 10 KB): **Status** (INFO), **Log** (LOG 20), **Modules** (SWEEP), **Network**
  (sys.name, net.dhcp/ip/mask/gw/dns/ntp: HELLO, SET, SAVE, then REBOOT with a confirm).
- `GET /api?l=<console line>` runs one `$LGS` line through `gwConsole_execute()` and returns its lines as text/plain.
  Same 1-120 printable-character gate as the unit 255 tunnel. Anything else: 404.

### SWEEP
- `$LGS SWEEP START [ids]` / `STATUS` / `STOP`, read-only, no HELLO. One module per `loop()` pass: FC03 regs 0-1, one
  retry (the hub crossing frame is lost by design). Ids from the panel layout, else the hub map rows, else 11-108.
  `STATUS` gives `state done total ok slow silent` and one `#DATA id= st=ok|slow|silent ms= fw=` per module.
  Its frames are foreign traffic, so a background module update yields to it.

### Events and INFO
- Events (appended): `GW_EV_AP = 20` (aux 1 on (param = who: 1 console, 2 panel), 2 off (param = minutes up),
  3 start failed, 4 refused (param = why)), `GW_EV_WIFI_FW = 21` (aux 1 installed, 2 CRC mismatch).
- `INFO`: `ap.state=off|starting|on|failed ap.clients=n ap.left_s=n ap.bridge=0|1 ap.heap=<free bytes>`.

## Test plan on Chest-Std-11 (network only: there is no USB Opta on this PC)

Std-11 is reached only over the LAN, so every step keeps the boot path unchanged and puts the new code behind a command.
This PC's own Wi-Fi carries its route to the bench and its internet, so it **must not join the AP**; it is also too far
from the cabinets to hear the AP at all (a scan on 2026-11-05 saw only its own network). So the PC checks the gateway's
side and the LAN, and the radio side (the SSID on air, association, DHCP, the page) needs a phone or another Opta WiFi
near the cabinet (`AP PROBE` on a second gateway does it end to end). `AP SCAN`, `AP PROBE` (which reboots the unit) and
`AP TESTLAN 1` (the page on the LAN too) exist only in a bench build, `-DWIFIAP_TEST=1`; a normal build has none of them.

| # | Step | Pass when |
|---|---|---|
| A1 | Flash build A (model + radio-file status only) | gateway back, `hw.*` and `wifi.fw` in INFO, settings unchanged |
| A2 | Read `hw.wifi` | 1 = Std-11 is an Opta WiFi; 0 = stop the radio tests, keep the code |
| B1 | Install the radio file over the network | `wifi.fw=ok`, `wifi_fw aux=1` in the log, event log intact (LOG SINCE continues) |
| C1 | `AP PASS`, `AP ON min=15` | `ap.state=on`, SSID `LGS-Chest-Std-11` WPA2 seen in a scan, Modbus over the LAN unaffected |
| C2 | `AP OFF`, then 50 x ON/OFF | every cycle on and off, no reset, `ap.heap` stable |
| C3 | AP on during a TCP soak and the event collector | 0 failed reads, 0 resets |
| D1 | `GET /` from the LAN | no answer, connection closed (`tcp_close p=1`) |
| D2 | SWEEP over the console | 40/40 ok on Std-11 |
| E | Overnight: AP on (min=480) + soak | 0 watchdog resets, `sys.up` grows, soak clean |
| P | Morning, with a phone | joins, gets 192.168.77.x, page loads, Status/Log/Modules/Network work |

## Not in this change

- The boot-button bug (`sampleBootButton()` takes `idle` from the first sample, so a hold at power-up is never seen):
  fixing it changes what a held button does at boot, and on a remote-only bench a wrong read would force defaults and
  take Ethernet off. Fix it with a unit on USB.
- Test Tool support (install button, AP password, status): after the firmware side is proven.

## Risks

- Ethernet and the AP share one 16 KB lwIP heap and one socket table; measured in C2/C3/E.
- `beginAP` after a `disconnect()` in the same boot is reported flaky on the mbed core (ArduinoCore-mbed issue #1040);
  C2 measures it. If it fails, ON after OFF answers "reboot first".
- The radio's file system mount (`wlan`) runs inside the driver with its own QSPIF instance; our own QSPI users
  (config store, event log, OTA partition) must keep working while the AP is up: C3 and E watch it.
- Hospital IT policy on an extra access point.
