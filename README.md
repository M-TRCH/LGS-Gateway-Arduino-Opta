# LGS-Gateway-Arduino-Opta

Transparent **Modbus TCP → Modbus RTU gateway** running on an [Arduino Opta](https://docs.arduino.cc/hardware/opta/). It accepts Modbus TCP requests on port 502, re-frames them as Modbus RTU (strips the MBAP header, appends CRC-16), forwards them over the RS485 bus, and returns the slave's reply to the TCP client with the original transaction ID.

It is also the cabinet's own control panel: five buttons and four relay outputs on the Opta's terminals let an LGS cabinet be exercised, watched and power-cycled at the cabinet, with no PC and no network.

Everything below is **runtime configuration** held in the Opta's own flash and edited over the `$LGS` text console on the USB port — usually from the [LGS Test Tool](https://github.com/M-TRCH/LGS-Test-Tool)'s Gateway tab. One firmware build serves every site.

## Hardware

| Item | Detail |
|---|---|
| Board | Arduino Opta (STM32H747, mbed core) |
| RS485 | Built-in half-duplex port (A/B terminals) → Modbus RTU bus @ 9600 baud |
| Ethernet | Built-in RJ45, static IP by default |
| Relay outputs | `D0`–`D3` = Opta outputs 1–4. What each one does is mapped at `panel.out1`–`out4`; by default O1 carries the shelf's power and O2–O4 are status lamps |
| Buttons (inputs) | `A0`–`A4` = Opta inputs 1–5. What each one does is mapped at `panel.btn1`–`btn5`; the cabinet's are wired red, green, blue, yellow, white in that order |
| Clock | On-board RTC with **no battery** — the time is lost on every power cut and must be set again (the Test Tool does it automatically) |

## Front panel

### Buttons (inputs 1–5)

Each button runs its action across the whole cabinet, one slot at a time from `loop()` so the bridges and the watchdog keep running. Pressing another button replaces whatever is running.

| Action (`panel.btnN`) | What it does |
|---|---|
| `0` none | Button ignored |
| `1` all_on | Ring + slot number on every slot, slot by slot, left on |
| `2` all_off | Everything out |
| `3` all_unlock | Ring + number + latch on every slot |
| `4` reset | Drops the shelf's power for `panel.reset_ms` — a power cycle |

Defaults follow the cabinet as built: red = all_on, green = all_off, blue = all_unlock, white = reset, yellow unassigned. `panel.enabled` is **off** by default, so a bench unit with nothing wired to its inputs cannot sweep somebody's cabinet.

Which slots a sweep walks comes from `panel.cabinet` (40, 64 or 80 — the catalogue shapes, hard-coded). A cabinet that is none of those sets **`panel.shape`** instead: slots per row from the top, `8,8,8,4,4,4,4,8,8,8` style, up to ten rows of 0–8. When the shape is non-zero it overrides the preset; `0` clears it. The Test Tool's header cabinet picker keeps both in step and warns when they disagree.

How a lit slot *looks* — brightness and colour — is **`panel.preset`** (1–8, default 1): the sweeps fire that preset's coils (`1010+p` ring + display, `1030+p` plus the latch), and the preset's brightness and colour are per-module configuration. A panel that is too bright at night is fixed by pointing it at a dimmer preset, not by a firmware change. Each sweep kind has its own per-slot pace — `panel.step_on_ms`, `panel.step_off_ms`, `panel.step_unlock_ms` — because lighting can walk slowly for show, clearing can be instant, and the unlock pace is really a power budget that spaces the solenoid firings out.

For a quick test there is also **`panel.bright`** (0–100, default 0 = off): when set, the sweep writes each module's *volatile* global brightness (reg 190) before lighting it — the whole cabinet lights at that brightness for the test, and a power cycle restores the configured look because nothing was stored. One extra write per slot while set.

### Relay outputs (1–4)

Each output follows one source. Ready, busy and fault are three faces of one state, so mapping those to three outputs gives a traffic light — exactly one lit, worst news first.

| Source (`panel.outN`) | Output is energised when |
|---|---|
| `0` none | never |
| `1` ready | no fault, and nothing is talking to the cabinet |
| `2` busy | RS485 traffic within `panel.lamp_hold_ms`, or a sweep is running |
| `3` fault | resetting, safe mode, store unavailable, LAN configured but down, or the bus has stopped answering (`panel.lamp_dead` consecutive timeouts) |
| `4` link | the LAN is up |
| `5` client | a Modbus TCP client is connected |
| `6` sweep | a panel sweep is running |
| `7` reset | the shelf's power is dropped right now |
| `8` shelf | **the shelf's power** — energised except while a reset runs |

These are mechanical relays, not LEDs. Traffic *holds* a lamp for a window rather than flashing it, and no lamp changes state faster than `panel.lamp_dwell_ms`; under a server polling steadily the busy lamp simply stays on. An output mapped to `shelf` is exempt from all of that — it is not rate-limited, it is not switched off with the lamps, and a lamp test never touches it, because cutting the cabinet to check a bulb would be a poor trade.

`$LGS LAMP 1|2|3|4|off [ms]` drives one output directly so the panel's wiring can be checked without waiting for the gateway to feel like being green. It expires on its own.

## Clock and scheduled reset

The Opta's RTC has no battery, so the time is lost on every power cut — the very event a scheduled reset exists to recover from. Therefore:

- the clock is **unset** until somebody sets it, and `time.set` says which;
- **nothing is ever scheduled while it is unset**, because a gateway that booted believing it was January 1970 would fire the moment its target came round in that fiction;
- the Test Tool sets it whenever it reads a gateway whose clock is unset, so in practice it is right within seconds of a power-up.

The clock keeps **wall time, not UTC**. There is no timezone anywhere in this firmware: a schedule that says 03:00 means the 03:00 the pharmacist would recognise, and a tool setting it must send local seconds.

```
$LGS TIME              read the clock
$LGS TIME 1786380152   set it (seconds since 1970, LOCAL)
```

There are **four times**, each armed by its own bit in `sched.reset_slots` (bit 0 = slot 1), all sharing one set of days and one master switch — a site wanting a single nightly reset arms one, a site wanting one per shift arms four. A disarmed slot keeps its time, so turning it back on later does not mean typing the hour again.

| Key | Meaning |
|---|---|
| `sched.reset_enabled` | the master switch for all four |
| `sched.reset_hhmm`, `_hhmm2`, `_hhmm3`, `_hhmm4` | slots 1–4, as a clock reading (300 = 03:00) |
| `sched.reset_slots` | which slots are armed, bit 0 = slot 1 |
| `sched.reset_days` | bit 0 = Sunday, 0 = every day |

They fire the same path the reset button takes. `sched.last` in `INFO` answers "did it actually run?", which matters for something that is over in a second and a half at three in the morning; `sched.reset` reads back as `03:00,15:00@Mon,Wed`, or `on_but_no_slots` when the switch is on and nothing is ticked — a state worth saying out loud rather than showing as "off".

## Watchdog

The board resets itself if `loop()` stops running for `sys.wdt_ms` (default 8000). That is well clear of the longest legitimate stall — a cross-channel RS485 hold, about 2.2 s — and short enough to bring a wedged gateway back before anyone walks to the cabinet. The reset reason then reads `sys.reset=watchdog`, and three failed boots in a row drop the unit into safe mode on factory defaults, so a stored value that hangs the board heals itself with no tooling.

It starts **partway through `setup()`**, as soon as the config has loaded, rather than at the end: everything after that point can block — the QSPI store, the first bus traffic, and above all the boot-time wait for an Ethernet link — and a board that wedges in `setup()` never reaches `loop()` at all. For the same reason the period has to outlast that boot-time wait, so `SAVE` refuses a `sys.wdt_ms` shorter than `net.link_timeout_ms` (plus the DHCP allowance when DHCP is on) rather than let the gateway reset itself forever while waiting for its own LAN.

A change takes effect on the next restart: the STM32's independent watchdog cannot be reconfigured once it is running. `INFO` reports `sys.wdt` — the period *actually* running, which is the compiled default if the hardware refused the stored one.

## RS485 switch hub

A cabinet may route RS485 through a channel-switching hub. Measured on a live LGS-64: the first frame on a new channel triggers the switch, that frame is always swallowed, and the channel stays deaf for about **two seconds** — longer than any master's timeout, so no amount of retrying inside one transaction can save it.

The gateway therefore repairs by **clock, not burst**: it spends the trigger frame, remembers when the channel opens (`bus.hub_settle_ms`), and holds any earlier request in silence until that deadline before sending it. `bus.hub_map` says which row hangs off which channel — wiring, not arithmetic — and an all-zero map disables the whole thing, which is what a gateway wired straight to the bus wants.

What a master must do for a full sweep to succeed:

| Master behaviour | Result on a 64-slot cabinet |
|---|---|
| retries ≥ 1, timeout 1.5 s | **64/64** — the retry lands in the hold |
| no retries, timeout 1.5 s | 55/64 — only the trigger frame per channel change is lost |
| no retries, timeout ≥ 2.6 s, `bus.hub_budget_ms` ≈ 3000 | **64/64** — repaired in-line |

`bus.hub_budget_ms` must stay under the master's timeout: overrunning it desynchronises the bridge, which is worse than the frame the repair set out to save.

## Network defaults

| Setting | Default | Key |
|---|---|---|
| Ethernet | **off** | `net.enabled` |
| Static IP | `192.168.0.178` | `net.ip` |
| Modbus TCP port | `502` | `net.port` |
| MAC address | read from the STM32's OTP | *(read-only, `net.mac`)* |

Ethernet is off out of the box: a gateway shipped for bench use talks over USB only, and a unit that has never been configured should not put an address on someone's LAN. The server accepts **one TCP client at a time** — a second connection is silently closed, which looks exactly like a dead gateway if you do not know it.

## Console

Text lines on the USB port, `$LGS ` prefixed, so the same cable carries Modbus and configuration without a mode switch.

```
$LGS PING | INFO | HELP | GET [key] | LAMP <1-4|off> [ms] | TIME [epoch]
$LGS HELLO            arm a 120 s session (required by the write verbs)
$LGS SET key=value …  stage changes
$LGS SAVE             validate, persist, apply live
$LGS DISCARD | DEFAULTS | REBOOT | BYE
```

Write verbs need an armed session so a false positive from binary Modbus traffic can never mutate anything. `SAVE` reports which groups took effect (`applied=bus,panel`) and which keys need a reboot.

Stored settings carry a **schema number**, checked on load. Bump `GW_BLOB_SCHEMA` in `src/gw_store.cpp` whenever a field's *meaning* changes without the struct changing size — otherwise the old bytes are read back as the new field and quietly mean something else.

## Build / upload / monitor

PlatformIO project (VS Code + PlatformIO IDE extension recommended — see `.vscode/extensions.json`).

```
pio run                # build
pio run -t upload      # flash over USB
pio device monitor     # serial log @ 115200 baud (sys.log must be on)
```

The Test Tool's Gateway tab can also flash a `.bin` over DFU, and prepare a factory-fresh Opta whose QSPI has no partition table.

## Project structure

```
include/config.h      Compile-time DEFAULTS only — everything a site changes lives in GwConfig
include/gw_config.h   The settings struct, the key table's contract
src/gw_config.cpp     Key table, parse/format, validation, apply-live
src/gw_store.cpp      Persistence in the QSPI KVStore: magic, schema, size, CRC
src/gw_console.cpp    The $LGS line protocol
src/modbus_rtu.cpp    RTU transport, echo strip, and the RS485 hub settle clock
src/tcp_bridge.cpp    Modbus TCP server and TCP↔RTU re-framing
src/usb_bridge.cpp    USB→RS485 passthrough
src/panel.cpp         Buttons, cabinet sweeps, relay outputs and lamps
src/sched.cpp         Wall clock and the scheduled shelf reset
src/gw_status.cpp     Counters, status LEDs, reset reason, OTP MAC
src/main.cpp          Setup ordering and the loop
```

## Provenance

Cloned from [`M-TRCH/LGS-Master`](https://github.com/M-TRCH/LGS-Master) branch `Modbus-Gateway` @ `70eb03f`; git history restarted for standalone development.

---

## สรุปภาษาไทย

เฟิร์มแวร์ **Gateway แปลง Modbus TCP ↔ Modbus RTU** บน Arduino Opta — รับคำสั่ง Modbus TCP ทาง Ethernet (port 502, รับทีละ 1 client) แล้วส่งต่อไปยังโมดูลบนบัส RS485 และเป็นแผงควบคุมหน้าตู้ด้วย: ปุ่ม 5 ปุ่มกับรีเลย์ 4 ตัวบนขั้วของ Opta ทำให้ทดสอบ ดูสถานะ และตัดไฟตู้ได้ที่หน้าตู้เลย ไม่ต้องมีคอมพิวเตอร์และไม่ต้องมีเครือข่าย

**ทุกอย่างตั้งค่าตอนใช้งานได้** ผ่านคำสั่งข้อความ `$LGS` บนสาย USB (ปกติสั่งจากแท็บ Gateway ของ LGS Test Tool) เฟิร์มแวร์ตัวเดียวจึงใช้ได้ทุกหน้างาน — ปุ่มไหนทำอะไร รีเลย์ไหนเป็นไฟสีอะไรหรือเป็นไฟเลี้ยงชั้นวาง ผังสาย RS485 hub ไปจนถึงเวลารีเซตอัตโนมัติ ล้วนเป็นค่าตั้งทั้งหมด

ข้อควรรู้สามเรื่อง:

**hub สลับช่องต้องเงียบราว 2 วินาที** เกตเวย์จึงหน่วงคำขอไว้เงียบๆ จนช่องพร้อมแทนการยิงซ้ำรัว (ฝั่งเซิร์ฟเวอร์ต้องตั้ง retry ≥ 1 หรือ timeout ≥ 2.6 วิ)

**นาฬิกาของ Opta ไม่มีแบตเตอรี่** เวลาหายทุกครั้งที่ไฟดับ ระบบจึงไม่ยิงตารางใดๆ จนกว่าจะมีคนตั้งเวลา (Test Tool ตั้งให้เองอัตโนมัติ) และนาฬิกาเก็บเวลาท้องถิ่น ไม่ใช่ UTC ตารางรีเซตตั้งได้ **4 ช่วงต่อวัน** ติ๊กเปิดเฉพาะช่วงที่ต้องการ ช่วงที่ปิดไว้ยังเก็บเวลาเดิม

**watchdog** จะรีเซตบอร์ดถ้าเฟิร์มแวร์หยุดทำงานเกิน `sys.wdt_ms` (ค่าเริ่มต้น 8000 มิลลิวินาที) เริ่มทำงานตั้งแต่กลาง `setup()` จึงครอบคลุมช่วงรอลิงก์ Ethernet ที่เป็นจุดค้างได้จริง — และด้วยเหตุนี้ค่าที่ตั้งต้องมากกว่าเวลารอลิงก์ ไม่งั้นบอร์ดจะรีเซตตัวเองวนไม่จบ (ระบบตรวจให้แล้วตอน SAVE) การเปลี่ยนค่ามีผลเมื่อรีสตาร์ท เพราะ IWDG ของ STM32 ตั้งใหม่ระหว่างทำงานไม่ได้
