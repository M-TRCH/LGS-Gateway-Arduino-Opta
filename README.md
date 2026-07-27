# LGS-Gateway-Arduino-Opta

Transparent **Modbus TCP → Modbus RTU gateway** running on an [Arduino Opta](https://docs.arduino.cc/hardware/opta/). It accepts Modbus TCP requests on port 502, re-frames them as Modbus RTU (strips the MBAP header, appends CRC-16), forwards them over the RS485 bus, and returns the slave's reply to the TCP client with the original transaction ID.

Acts as the network bridge for the LGS module grid, where slaves are addressed by cabinet position.

## Hardware

| Item | Detail |
|---|---|
| Board | Arduino Opta (STM32H747, mbed core) |
| RS485 | Built-in half-duplex port (A/B terminals) → Modbus RTU bus @ 9600 baud |
| Ethernet | Built-in RJ45, static IP |
| Relay outputs | `D0` = module power relay, `D1` = LED power relay (both driven HIGH at boot) |
| Buttons (inputs) | `A0` Red, `A1` Green, `A2` Blue, `A3` Yellow, `A4` White (Opta terminals I1–I5) |

## Network defaults

| Setting | Value | Where to change |
|---|---|---|
| Static IP | `192.168.0.178` | `include/config.h` (`NET_STATIC_IP`) |
| MAC address | `DE:AD:BE:EF:FE:ED` | `include/config.h` (`NET_MAC_ADDRESS`) |
| Modbus TCP port | `502` | `include/config.h` (`MODBUS_TCP_PORT`) |

The server accepts **one TCP client at a time**; additional connections are refused until the active client disconnects.

## Front-panel buttons

| Button | Action |
|---|---|
| White (`A4`) | Hardware reset — relays off, 3 s settle, MCU reset |
| Red (`A0`) | Coil sweep self-test (~10 s; longer if slaves don't respond) |
| Blue (`A2`) | Extended coil test (~72 s) |
| Green / Yellow | Not assigned yet |

Self-tests iterate the module grid rows 1–6 × columns 1–4 with slave ID = `row*10 + col` (e.g. row 2, column 3 → slave 23), writing coil 1004 (sweep) / 1024 (extended). A coil sweep also runs automatically ~2 s after boot. **While a self-test runs, the TCP bridge is not serviced** — clients see no responses until it finishes.

## Key configuration (`include/config.h`)

| Define | Default | Meaning |
|---|---|---|
| `RS485_BAUD` | 9600 | RTU bus baud rate |
| `TIMEOUT_FIRST_BYTE_MS` | 300 | Wait for first byte of the slave reply |
| `TIMEOUT_INTER_BYTE_MS` | 20 | RTU inter-byte frame gap |
| `TIMEOUT_TCP_PAYLOAD_MS` | 100 | Wait for fragmented TCP payload |
| `RTU_BUF_SIZE` / `TCP_BUF_SIZE` | 256 | Frame buffers |
| `SELFTEST_ROWS` / `SELFTEST_COLS` | 6 / 4 | Module grid size for self-tests |

## Build / upload / monitor

PlatformIO project (VS Code + PlatformIO IDE extension recommended — see `.vscode/extensions.json`).

```
pio run                # build
pio run -t upload      # flash over USB
pio device monitor     # serial log @ 115200 baud
```

Equivalent VS Code tasks **Build / Upload / Monitor** are predefined in `.vscode/tasks.json`.

## Project structure

```
include/config.h                            All tunables: pins, network identity, baud rates, timeouts, self-test grid
include/modbus_rtu.h + src/modbus_rtu.cpp   RTU transport: CRC-16, transaction with echo-strip, FC05 writeCoil
include/tcp_bridge.h + src/tcp_bridge.cpp   Modbus TCP server (port 502) and TCP↔RTU re-framing
include/self_test.h  + src/self_test.cpp    Coil sweep + extended coil test routines
src/main.cpp                                Setup, button dispatch, hardware reset
```

## Publishing to GitHub (when ready)

The repo is local-only for now. To publish under the M-TRCH account:

```powershell
gh auth login   # first time only
gh repo create M-TRCH/LGS-Gateway-Arduino-Opta --private --source=. --remote=origin --push
```

Use `--public` instead of `--private` if preferred. Manual alternative:

```powershell
git remote add origin https://github.com/M-TRCH/LGS-Gateway-Arduino-Opta.git
git push -u origin main
```

## Provenance

Cloned from [`M-TRCH/LGS-Master`](https://github.com/M-TRCH/LGS-Master) branch `Modbus-Gateway` @ `70eb03f`; git history restarted for standalone development.

---

## สรุปภาษาไทย

เฟิร์มแวร์ **Gateway แปลง Modbus TCP ↔ Modbus RTU** บน Arduino Opta — รับคำสั่ง Modbus TCP ทาง Ethernet (port 502, รับทีละ 1 client) แล้วส่งต่อไปยังโมดูลบนบัส RS485 (9600 baud) โดยแปลง frame ให้อัตโนมัติ ปุ่มหน้าเครื่อง: ขาว = รีเซ็ตฮาร์ดแวร์, แดง = ทดสอบ coil sweep, น้ำเงิน = ทดสอบแบบยาว (ระหว่างทดสอบ bridge จะหยุดรับ TCP ชั่วคราว) หลังบูต ~2 วินาทีจะรัน coil sweep อัตโนมัติหนึ่งรอบ build/upload ด้วย PlatformIO
