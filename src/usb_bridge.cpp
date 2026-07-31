#include "usb_bridge.h"
#include "gw_console.h"
#include "gw_status.h"
#include "modbus_rtu.h"

static uint16_t _gap_ms = DEF_USB_FRAME_GAP_MS;
static uint16_t _max_ms = DEF_USB_FRAME_MAX_MS;

void usbBridge_setFraming(uint16_t gapMs, uint16_t maxMs) {
    _gap_ms = gapMs;
    _max_ms = maxMs;
}

// ── Entry ──────────────────────────────────────────────────────────────────
void usbBridge_begin() {
    while (Serial.available()) Serial.read();   // drop stale host bytes
}

// ── Update (call every loop) ───────────────────────────────────────────────
void usbBridge_update() {
    if (!Serial.available()) return;

    // Accumulate one RTU frame: the frame ends after _gap_ms of silence. USB
    // CDC delivers bytes in bursts, so a fixed gap stands in for the T3.5
    // timing a real serial line would provide.
    uint8_t frame[RTU_BUF_SIZE];
    int len = 0;
    unsigned long t_start = millis();
    unsigned long t_last  = t_start;

    while (millis() - t_last < _gap_ms) {
        if (millis() - t_start > _max_ms) break;   // runaway stream guard
        if (Serial.available()) {
            uint8_t b = (uint8_t)Serial.read();
            if (len < RTU_BUF_SIZE) frame[len++] = b;
            t_last = millis();
        }
    }

    // Forward only complete, CRC-valid requests (min frame: addr+FC+CRC = 4).
    // Everything else was already being thrown away here, so handing it to the
    // console cannot affect Modbus traffic: a well-formed frame never reaches
    // the parser. The console stays silent unless it assembles a complete,
    // valid command.
    if (len < 4 || !verifyCRC(frame, len)) {
        gwStatus_count(GW_USB_DROP);
        gwConsole_feed(frame, len);
        return;
    }
    gwStatus_count(GW_USB_OK);
    printHex("USB TX->RS485", frame, len);

    // Broadcast (slave address 0, e.g. the OTA chunk stream): no slave ever
    // answers, so forward it and move on. Waiting the full first-byte timeout
    // would stall this loop while the host's next frames accumulate in the USB
    // buffer — they would then be read back-to-back as one oversized frame,
    // fail CRC and be dropped.
    if (frame[0] == 0x00) {
        rtu_send(frame, len);
        gwStatus_pulseRs485();
        return;
    }

    uint8_t rx[RTU_BUF_SIZE];
    unsigned long t0 = millis();
    int rx_len = rtu_transact(frame, len, rx);
    gwStatus_pulseRs485();

    // Return the reply verbatim — the PC master does its own CRC check.
    if (rx_len > 0) {
        gwStatus_countRtu(true, millis() - t0);
        printHex("RS485->USB", rx, rx_len);
        Serial.write(rx, rx_len);
    } else {
        gwStatus_countRtu(false, millis() - t0);
        LOG_SERIAL.println("[USB] no reply from RS485");
    }
}
