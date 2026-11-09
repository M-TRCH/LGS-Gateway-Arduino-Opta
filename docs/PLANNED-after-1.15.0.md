# Planned after 1.15.0

Found by the release review of 2026-11-09 (v1.14.0...ap-show). **None of these block 1.15.0.** The site checkpoint
(Central Chest Institute, 12 cabinets) runs the build that was tested on Std-11; fixing any of these changes that build and
would need the bench tests and the soak again, so they wait for the next version. Each item says how it is kept under
control until then.

The 1.15.0 image is the one Std-11 has run since the evening of 2026-11-09 (built 20:42): commit 561495f, 346,644 bytes, SHA-256
`bfa0e64a02d2151553ead08479c7c280297aa930c3a63dc25f4957f778b40570` (CRC-32 `0x43dd080b`), staged as
`assets/gateway_opta_v1.15.0.bin`. A rebuild on another day differs only in the `__DATE__` build string; flash this file,
not a rebuild.

สรุป: ข้อที่พบตอนตรวจก่อนปล่อย 1.15.0 ไม่บล็อก release เพราะควบคุมได้ด้วยวิธีใช้งาน และแก้ตอนนี้จะทำให้ผลทดสอบที่ผ่านมาใช้ไม่ได้
จึงเก็บไว้แก้ในเวอร์ชันถัดไป

## 1. A service-AP client can hold a TCP client slot until the gateway reboots

`tcp_bridge.cpp` has two client slots and no idle timeout. A connection from the AP subnet (192.168.27.x) that never
closes cleanly stays in its slot: a phone that preconnects and stays idle, a phone that walks out of range, or any phone
still connected when the AP is switched off or idles off (no FIN ever arrives once the radio is gone). With the Test Tool
in the other slot, the hospital server's Modbus poller is then refused (`tcp_refused` in the event log) until the gateway
restarts.

- **Until fixed:** the AP is off unless a technician switches it on, and goes off by itself after its idle minutes. Close
  the browser tab before leaving the cabinet. If the LAN poller is refused after AP use, reboot the gateway (`$LGS REBOOT`
  or power).
- **Fix:** when the AP stops, `stop()` every slot whose peer is on the AP subnet; and give AP-subnet slots an idle timeout
  (e.g. 60 s without a byte). LAN clients keep today's behaviour: the hospital poller may legitimately idle.
- **Test:** phone joins, opens the page, AP OFF from the LAN: `net.client` drops back; phone left idle on the page: its slot
  frees after the timeout; LAN soak unaffected.

## 2. The AP page can unlock a drawer although the AP refuses Modbus from its clients

`tcp_bridge.cpp` answers bus requests from AP clients with exception 0x0A unless the AP was started with `bridge=1`,
precisely so that a phone on the AP cannot open drawer latches. `MOD <id> UNLOCK` (mod_sweep.cpp) is a console verb, and
the console is open to AP clients, so the page's "ปลดล็อก" button drives coil 1030+p anyway. The AP password is printed on
the "Standard + AP Service" sticker.

- **Until fixed:** treat the AP password like a key to the cabinet. Do not use the AP-Service sticker (or hand out the
  password) on a narcotic cabinet; keep the AP off outside service visits.
- **Fix:** refuse `MOD ... UNLOCK` when the console line came from an AP client and the AP is not in `bridge=1` (the AP
  page then hides the button), or drop UNLOCK from the AP page entirely. LIGHT / OFF stay.
- **Test:** from a phone on the AP with bridge=0: UNLOCK refused, LIGHT/OFF work; with `AP ON bridge=1`: UNLOCK works; from
  the LAN console: unchanged.

## 3. MOD FIND / SETID only reach a board on the hub channel the hub is parked on

Ids 246 and 247 belong to no hub row, so `rtu_transact` does not switch the hub for them: FIND and SETID talk to whatever
channel the last request moved the hub to. On a hub cabinet a factory-reset board plugged into its slot is found only if
the hub happens to be on that slot's channel. The bench test on Std-11 passed because the previous command had touched
row 10, where the board (module 104) sat.

- **Until fixed:** set a new board's id with the Test Tool's module page, or on a direct (no-hub) bus; the AP page's
  "บอร์ดใหม่" card is reliable only on cabinets without a hub map.
- **Fix:** SETID probes the target id first (that parks the hub on the target slot's channel, which is where the board
  physically is), then the source id. FIND takes an optional slot (`MOD FIND 53`) and the page asks "which slot is the new
  board in?" before searching.
- **Test:** Std-11, a board at 247 on row 5's channel with the hub parked on row 10: FIND 53 finds it, SETID 247 → 53
  succeeds.

## Also found (lower priority)

- `wifi_ap.cpp` wFinish removes the old `4343WA1.BIN` before renaming the verified temp file: a failed rename leaves no
  radio file. Rename over it, or keep the old one until the new name is in place.
- `ap_http.cpp` waits up to 1.5 s for request headers inside `loop()`; the bridges stall for that long on a slow phone.
- The AP page's HTTP close is logged `tcp_close p=0` ("client went away"); event_log.h defines p=1 for "closed by the
  gateway".
- `mod_sweep.cpp` setidStep uses `millis() < _siNext` (not wrap-safe at 49.7 days of uptime).
- The "AP refused" event (`service_ap a=4`) is logged with p=0 instead of the reason.
- FC03/05/06 frame building is repeated in mod_sweep.cpp and panel.cpp; a small `rtu_readReg/rtu_writeReg/rtu_writeCoil`
  in modbus_rtu would replace the copies.

Reviewed and **not** a problem: `OTA START at=… until=…` with the clock unset is already refused (`err=clock_unset`,
ota_runner.cpp cmdStart), and a power cut that loses the clock also ends the run, which lives in RAM.
