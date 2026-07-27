#include "usb_bridge.h"
#include "modbus_rtu.h"

// ── Mode entry / exit ──────────────────────────────────────────────────────
void usbBridge_begin() {
    rtu_setQuiet(true);
    while (Serial.available()) Serial.read();   // drop stale host bytes
}

void usbBridge_end() {
    rtu_setQuiet(false);
}

// ── Update (call every loop while active) ──────────────────────────────────
void usbBridge_update() {
    if (!Serial.available()) return;

    // Accumulate one RTU frame: the frame ends after USB_FRAME_GAP_MS of
    // silence. USB CDC delivers bytes in bursts, so a fixed gap stands in
    // for the T3.5 timing a real serial line would provide.
    uint8_t frame[RTU_BUF_SIZE];
    int len = 0;
    unsigned long t_start = millis();
    unsigned long t_last  = t_start;

    while (millis() - t_last < USB_FRAME_GAP_MS) {
        if (millis() - t_start > USB_FRAME_MAX_MS) break;   // runaway stream guard
        if (Serial.available()) {
            uint8_t b = (uint8_t)Serial.read();
            if (len < RTU_BUF_SIZE) frame[len++] = b;
            t_last = millis();
        }
    }

    // Forward only complete, CRC-valid requests (min frame: addr+FC+CRC = 4).
    // Anything else is dropped; the PC master will time out and retry.
    if (len < 4 || !verifyCRC(frame, len)) return;

    uint8_t rx[RTU_BUF_SIZE];
    int rx_len = rtu_transact(frame, len, rx);

    // Return the reply verbatim — the PC master does its own CRC check.
    // Broadcasts (addr 0) legitimately produce no reply.
    if (rx_len > 0) Serial.write(rx, rx_len);
}
