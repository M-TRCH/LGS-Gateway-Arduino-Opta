#include "modbus_rtu.h"

// Runtime log gate for the whole firmware (config.h declares it extern).
// Default OFF: the USB port must carry nothing but Modbus and console traffic
// unless someone deliberately turns logging on with `$LGS SET sys.log=1`.
bool g_logEnabled = false;

static uint16_t _t_first_ms = DEF_TIMEOUT_FIRST_BYTE_MS;
static uint16_t _t_inter_ms = DEF_TIMEOUT_INTER_BYTE_MS;

void rtu_setTimeouts(uint16_t firstByteMs, uint16_t interByteMs) {
    _t_first_ms = firstByteMs;
    _t_inter_ms = interByteMs;
}

// ── Debug ──────────────────────────────────────────────────────────────────
void printHex(const char* label, const uint8_t* buf, int len) {
    if (!g_logEnabled) return;              // skip the loop entirely when quiet
    LOG_SERIAL.print("[DBG] ");
    LOG_SERIAL.print(label);
    LOG_SERIAL.print(" (");
    LOG_SERIAL.print(len);
    LOG_SERIAL.print(" bytes): ");
    for (int i = 0; i < len; i++) {
        if (buf[i] < 0x10) LOG_SERIAL.print("0");
        LOG_SERIAL.print(buf[i], HEX);
        LOG_SERIAL.print(" ");
    }
    LOG_SERIAL.println();
}

// ── CRC-16/Modbus ──────────────────────────────────────────────────────────
uint16_t crc16(const uint8_t* buf, int len) {
    uint16_t crc = 0xFFFF;
    for (int i = 0; i < len; i++) {
        crc ^= (uint16_t)buf[i];
        for (int b = 8; b != 0; b--) {
            if (crc & 0x0001) { crc >>= 1; crc ^= 0xA001; }
            else               { crc >>= 1; }
        }
    }
    return crc;
}

bool verifyCRC(const uint8_t* buf, int len) {
    if (len < 3) return false;
    uint16_t calc = crc16(buf, len - 2);
    uint16_t recv = (uint16_t)buf[len - 2] | ((uint16_t)buf[len - 1] << 8);
    return calc == recv;
}

// ── RS485 Send-only (broadcast) ────────────────────────────────────────────
void rtu_send(const uint8_t* tx, int tx_len) {
    // Flush stale RX bytes before transmitting
    int flushed = 0;
    while (RS485.available() && flushed < RTU_BUF_SIZE) { RS485.read(); flushed++; }
    if (flushed > 0) {
        LOG_SERIAL.print("[RS485] Pre-TX flush: discarded ");
        LOG_SERIAL.print(flushed);
        LOG_SERIAL.println(" stale bytes.");
    }

    // endTransmission() blocks until the last byte has left the UART, so the
    // caller is paced by the bus itself.
    RS485.beginTransmission();
    RS485.write(tx, tx_len);
    RS485.endTransmission();
    RS485.receive();
}

// ── RS485 Transaction ──────────────────────────────────────────────────────
int rtu_transact(const uint8_t* tx, int tx_len, uint8_t* rx) {
    rtu_send(tx, tx_len);

    // Receive response with timeout
    int rx_len = 0;
    bool receiving = false;
    unsigned long t = millis();

    while (true) {
        if (RS485.available()) {
            uint8_t b = (uint8_t)RS485.read();
            if (rx_len < RTU_BUF_SIZE) {
                rx[rx_len++] = b;
                t = millis();
                receiving = true;
            }
        }
        unsigned long elapsed = millis() - t;
        if (!receiving && elapsed > _t_first_ms) break;
        if ( receiving && elapsed > _t_inter_ms) break;
    }

    // Strip TX echo if the transceiver looped back our own frame
    if (rx_len >= tx_len && memcmp(rx, tx, tx_len) == 0) {
        int rem = rx_len - tx_len;
        if (rem > 0) memmove(rx, rx + tx_len, rem);
        rx_len = rem;
    }

    return rx_len;
}
