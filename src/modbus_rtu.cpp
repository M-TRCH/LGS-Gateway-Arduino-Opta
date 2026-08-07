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

// ── RS485 switch hub ───────────────────────────────────────────────────────
// Set from gw_config; all zeros (the default) means no hub and disables every
// line of this. See gw_config.h for why the repair belongs to the gateway and
// not to whatever is speaking Modbus to it.
//
// The model, measured on a live LGS-64 cabinet: the first frame on a new
// channel is what makes the hub start switching, that frame is always lost,
// and the channel then stays deaf for about two seconds. Repair frames sent
// inside that window at 0.8 s, 1.15 s and 1.55 s were all eaten; the first
// request after ~2.0-2.5 s always went through. Two seconds is longer than
// any sane master timeout, so no amount of in-transaction retrying can save
// the trigger frame — and hammering the bus mid-settle made whole rows die
// (15/64) instead of one frame.
//
// So the repair is a clock, not a burst: remember WHEN the channel will be
// open (_hubReadyAt), spend the trigger frame cheaply, and for any request
// that arrives before the deadline HOLD it in silence until the deadline,
// then transmit once. A master that retries even once sends its retry into
// exactly that hold-and-send path and gets a clean reply — the row loses
// nothing. The budget caps how much of the master's patience one hold may
// consume; when the wait cannot fit, the gateway stays silent and fails the
// request rather than feed the hub another frame to swallow.
static uint8_t  _hubMap[GW_HUB_MAX_ROWS] = {0};
static uint8_t  _hubRetry = 1;          // attempts for the crossing frame itself
static uint16_t _hubGapMs = 0;          // margin added past the deadline
static uint16_t _hubSettleMs = DEF_HUB_SETTLE_MS;
static uint16_t _hubBudgetMs = 0;
static uint8_t  _hubLastCh = 0;         // channel the hub was left on
static uint32_t _hubReadyAt = 0;        // millis() when that channel opens
static uint32_t _hubCross = 0;          // transactions that crossed channels
static uint32_t _hubExtra = 0;          // extra frames those crossings cost
static uint32_t _hubWaitMs = 0;         // total ms spent holding for the settle
static uint32_t _hubSkip = 0;           // requests failed without touching the bus

void rtu_setHub(const uint8_t* map, uint8_t retry, uint16_t gapMs,
                uint16_t settleMs, uint16_t budgetMs) {
    if (map) memcpy(_hubMap, map, sizeof(_hubMap));
    _hubRetry    = retry ? retry : 1;
    _hubGapMs    = gapMs;
    _hubSettleMs = settleMs;
    _hubBudgetMs = budgetMs;
    _hubLastCh   = 0;                   // wiring changed: forget where we were
    _hubReadyAt  = millis();            // and drop any pending hold
}

uint32_t rtu_hubCross() { return _hubCross; }
uint32_t rtu_hubExtra() { return _hubExtra; }
uint32_t rtu_hubWaitMs() { return _hubWaitMs; }
uint32_t rtu_hubSkip() { return _hubSkip; }

// Channel a slave hangs off, or 0 when it is not behind the hub. Broadcast
// (id 0) lands here as row 0 and is deliberately never treated as a switch —
// nothing answers a broadcast, so retrying one would only cost time.
static uint8_t hubChannelOf(uint8_t slaveId) {
    const int row = slaveId / 10;
    if (row < 1 || row > GW_HUB_MAX_ROWS) return 0;
    return _hubMap[row - 1];
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
static int rtu_once(const uint8_t* tx, int tx_len, uint8_t* rx);

// Hold until the channel's settle deadline if the budget allows, then send.
// Returns -1 WITHOUT touching the bus when the wait cannot fit: a frame sent
// mid-settle is not just lost, it feeds the hub more traffic to swallow.
// The budget is measured from the start of the whole transaction, so the
// hold can never stretch a reply past what the master is willing to wait —
// overrunning that desynchronises the bridge, which is worse than the frame
// this code set out to save.
static int hubHoldAndSend(const uint8_t* tx, int tx_len, uint8_t* rx,
                          uint32_t startMs) {
    int32_t wait = (int32_t)(_hubReadyAt - millis());
    if (wait < 0) wait = 0;
    const uint32_t worst = (millis() - startMs) + (uint32_t)wait + _t_first_ms;
    if (_hubBudgetMs != 0 && worst > _hubBudgetMs) return -1;
    if (wait > 0) { delay(wait); _hubWaitMs += (uint32_t)wait; }
    return rtu_once(tx, tx_len, rx);
}

int rtu_transact(const uint8_t* tx, int tx_len, uint8_t* rx) {
    const uint8_t ch = (tx_len > 0) ? hubChannelOf(tx[0]) : 0;
    if (ch == 0) return rtu_once(tx, tx_len, rx);

    const uint32_t start = millis();
    // First-ever transaction counts as a crossing too: the hub could have
    // been left on any channel, so the safe assumption is that it moves.
    const bool crossed = (ch != _hubLastCh);
    _hubLastCh = ch;

    if (crossed) {
        _hubCross++;
        // This frame is the trigger: it is what makes the hub start moving,
        // so it must go out even though it is almost certainly lost.
        _hubReadyAt = millis() + _hubSettleMs + _hubGapMs;
        int rx_len = rtu_once(tx, tx_len, rx);
        if (rx_len > 0) {                   // answered anyway: channel was
            _hubReadyAt = millis();         // already open, nothing to wait out
            return rx_len;
        }
        // A patient master (budget raised accordingly) can ride out the whole
        // settle inside this one transaction; everyone else fails fast here
        // and gets repaired on their own retry via the hold path below.
        for (int i = 1; i < (int)_hubRetry; i++) {
            rx_len = hubHoldAndSend(tx, tx_len, rx, start);
            if (rx_len < 0) { _hubSkip++; return 0; }   // no room to wait
            _hubExtra++;
            if (rx_len > 0) return rx_len;
        }
        return 0;
    }

    // Same channel, but the hub may still be settling from a recent switch.
    // This is where a master's own retry of the lost trigger frame lands:
    // hold it in silence until the deadline, then send it once.
    if ((int32_t)(_hubReadyAt - millis()) > 0) {
        const int rx_len = hubHoldAndSend(tx, tx_len, rx, start);
        if (rx_len < 0) { _hubSkip++; return 0; }
        return rx_len;
    }

    return rtu_once(tx, tx_len, rx);
}

static int rtu_once(const uint8_t* tx, int tx_len, uint8_t* rx) {
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
