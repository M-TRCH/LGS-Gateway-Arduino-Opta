#include "gw_status.h"

#include "event_log.h"

#include <stm32h7xx_hal.h>
#include <drivers/Watchdog.h>

// The variant already owns an RTC handle and uses RTC_BKP_DR0 as the
// 1200 bps bootloader magic; DR1 upward are free.
extern RTC_HandleTypeDef RTCHandle;

#define BKP_MAGIC       0x4C475701UL      // 'LGW' + version
#define BKP_REG_MAGIC   RTC_BKP_DR1
#define BKP_REG_BOOTS   RTC_BKP_DR2

#define RS485_PULSE_MS  30
#define SAFE_MODE_BOOTS 3

static uint32_t _counters[GW_COUNTER_N];
static uint16_t _lastRtt, _maxRtt, _consecTimeouts;
static uint32_t _bootMs;
static uint8_t  _bootAttempts;
static bool     _safeMode;
static bool     _fault;
static const char* _resetReason = "unknown";
static uint8_t  _mac[6];
static bool     _macValid;
static uint32_t _lastRs485Ms = 0;
static unsigned long _rs485PulseUntil;
static uint16_t _wdtMs = 0;         // 0 until the watchdog is actually running

// ── Watchdog ───────────────────────────────────────────────────────────────
void gwStatus_watchdogBegin(uint16_t ms) {
    if (mbed::Watchdog::get_instance().start(ms)) { _wdtMs = ms; return; }
    // The IWDG can refuse a period the H7's prescaler cannot reach. Falling
    // back keeps a stored value from being able to disarm the watchdog.
    if (mbed::Watchdog::get_instance().start(DEF_WATCHDOG_MS)) {
        _wdtMs = DEF_WATCHDOG_MS;
        LOG_SERIAL.println("[SYS] sys.wdt_ms refused by the hardware — default used");
    } else {
        LOG_SERIAL.println("[SYS] watchdog unavailable");
    }
}

void gwStatus_watchdogKick() {
    if (_wdtMs) mbed::Watchdog::get_instance().kick();
}

uint16_t gwStatus_watchdogMs() { return _wdtMs; }

// A unicast address that is neither all-zero nor all-ones. Enough to tell a
// real OTP address from whatever happened to be at the fallback flash offset.
static bool macLooksReal(const uint8_t* m) {
    if (m[0] & 0x01) return false;                  // multicast bit
    uint8_t any = 0x00, all = 0xFF;
    for (int i = 0; i < 6; i++) { any |= m[i]; all &= m[i]; }
    return any != 0x00 && all != 0xFF;
}

// ── RTC backup domain ──────────────────────────────────────────────────────
static uint32_t bkpRead(uint32_t reg) {
    return HAL_RTCEx_BKUPRead(&RTCHandle, reg);
}

static void bkpWrite(uint32_t reg, uint32_t value) {
    HAL_PWR_EnableBkUpAccess();
    HAL_RTCEx_BKUPWrite(&RTCHandle, reg, value);
}

static uint8_t _resetCode = 0;      // GW_RST_* — the string, as a number the
                                    // event log can store in one byte

static void latchResetReason() {
    if      (__HAL_RCC_GET_FLAG(RCC_FLAG_IWDG1RST)) { _resetReason = "watchdog";        _resetCode = 1; }
    else if (__HAL_RCC_GET_FLAG(RCC_FLAG_WWDG1RST)) { _resetReason = "window-watchdog"; _resetCode = 2; }
    else if (__HAL_RCC_GET_FLAG(RCC_FLAG_SFTRST))   { _resetReason = "software";        _resetCode = 3; }
    else if (__HAL_RCC_GET_FLAG(RCC_FLAG_PINRST))   { _resetReason = "pin";             _resetCode = 4; }
    else if (__HAL_RCC_GET_FLAG(RCC_FLAG_BORRST))   { _resetReason = "brownout";        _resetCode = 5; }
    else if (__HAL_RCC_GET_FLAG(RCC_FLAG_PORRST))   { _resetReason = "power-on";        _resetCode = 6; }
    __HAL_RCC_CLEAR_RESET_FLAGS();

    // On the Opta the RCC flags are ALREADY CLEARED by the time the app
    // runs: the factory bootloader reads them first and parks its verdict —
    // an mbed reset_reason_t — in RTC backup DR8. Measured on the bench:
    // every path above missed, and the log's boot events all said 0. So
    // DR8 is the real source here; the flags stay as a first try in case a
    // bootloader change ever hands them back.
    if (_resetCode == 0) {
        switch (HAL_RTCEx_BKUPRead(&RTCHandle, RTC_BKP_DR8)) {
            case 0:  _resetReason = "power-on"; _resetCode = 6; break;
            case 1:  _resetReason = "pin";      _resetCode = 4; break;
            case 2:  _resetReason = "brownout"; _resetCode = 5; break;
            case 3:  _resetReason = "software"; _resetCode = 3; break;
            case 4:  _resetReason = "watchdog"; _resetCode = 1; break;
            default: break;                     // lockup/multiple/unknown
        }
    }
}

void gwStatus_begin() {
    _bootMs = millis();
    memset(_counters, 0, sizeof(_counters));
    _lastRtt = _maxRtt = _consecTimeouts = 0;

    latchResetReason();

    // Boot-attempt counter: incremented here, cleared by gwStatus_markHealthy()
    // once the loop has run healthily for a while. Three failed boots in a row
    // force safe mode, so a config that hangs the board self-heals.
    if (bkpRead(BKP_REG_MAGIC) != BKP_MAGIC) {
        bkpWrite(BKP_REG_MAGIC, BKP_MAGIC);
        bkpWrite(BKP_REG_BOOTS, 0);
    }
    _bootAttempts = (uint8_t)(bkpRead(BKP_REG_BOOTS) & 0xFF);
    if (_bootAttempts < 255) bkpWrite(BKP_REG_BOOTS, _bootAttempts + 1);
    _safeMode = (_bootAttempts >= SAFE_MODE_BOOTS);

    // Read the OTP MAC BEFORE anything mounts the QSPI block device: the
    // variant's OTP reader opens its own mbed::QSPI on the same pins.
    //
    // _getSecureEthMac_() returns the byte count (always 6), not a success
    // flag, and when the OTP magic does not match it copies from a raw flash
    // pointer instead — so the bytes are the only thing worth trusting.
    _macValid = (_getSecureEthMac_(_mac) == 6) && macLooksReal(_mac);
    if (!_macValid) {
        static const uint8_t fallback[6] = { 0xDE, 0xAD, 0xBE, 0xEF, 0xFE, 0xED };
        memcpy(_mac, fallback, sizeof(_mac));
    }

    pinMode(LED_RS485_PIN,   OUTPUT); digitalWrite(LED_RS485_PIN,   LOW);
    pinMode(LED_LINK_PIN,    OUTPUT); digitalWrite(LED_LINK_PIN,    LOW);
    pinMode(LED_SESSION_PIN, OUTPUT); digitalWrite(LED_SESSION_PIN, LOW);
    pinMode(LED_FAULT_PIN,   OUTPUT); digitalWrite(LED_FAULT_PIN,   LOW);
    pinMode(LED_TIMEOUT_PIN, OUTPUT); digitalWrite(LED_TIMEOUT_PIN, LOW);
    pinMode(GW_BUTTON_PIN,   INPUT_PULLUP);
}

// ── Counters ───────────────────────────────────────────────────────────────
void gwStatus_count(GwCounter c) {
    if (c < GW_COUNTER_N) _counters[c]++;
}

// How many transactions in a row must go unanswered before the bus counts
// as gone. The cabinet losing power silences every slave at once, so this
// trips within a second or two; one dead module can never reach it because
// the poll moves on to the next address.
#define BUS_QUIET_ENTER   6
static bool     _busQuiet = false;
static uint32_t _busQuietSince = 0;

void gwStatus_countRtu(bool replied, uint32_t rttMs) {
    if (rttMs > 0xFFFF) rttMs = 0xFFFF;
    _lastRtt = (uint16_t)rttMs;
    if (_lastRtt > _maxRtt) _maxRtt = _lastRtt;
    if (replied) {
        _counters[GW_RS485_OK]++;
        _consecTimeouts = 0;
        if (_busQuiet) {
            _busQuiet = false;
            eventLog_note(GW_EV_BUS_QUIET, 2,
                          (uint16_t)((millis() - _busQuietSince) / 1000UL));
        }
        digitalWrite(LED_TIMEOUT_PIN, LOW);
    } else {
        _counters[GW_RS485_TIMEOUT]++;
        if (_consecTimeouts < 0xFFFF) _consecTimeouts++;
        if (!_busQuiet && _consecTimeouts >= BUS_QUIET_ENTER) {
            _busQuiet = true;
            _busQuietSince = millis();
            eventLog_note(GW_EV_BUS_QUIET, 1, _consecTimeouts);
        }
        digitalWrite(LED_TIMEOUT_PIN, HIGH);
    }
}

uint32_t gwStatus_get(GwCounter c)   { return c < GW_COUNTER_N ? _counters[c] : 0; }
uint16_t gwStatus_lastRttMs()        { return _lastRtt; }
uint16_t gwStatus_maxRttMs()         { return _maxRtt; }
uint16_t gwStatus_consecutiveTimeouts() { return _consecTimeouts; }

void gwStatus_resetCounters() {
    memset(_counters, 0, sizeof(_counters));
    _lastRtt = _maxRtt = _consecTimeouts = 0;
    digitalWrite(LED_TIMEOUT_PIN, LOW);
}

// ── Indicators ─────────────────────────────────────────────────────────────
void gwStatus_pulseRs485() {
    digitalWrite(LED_RS485_PIN, HIGH);
    _lastRs485Ms = millis();
    _rs485PulseUntil = _lastRs485Ms + RS485_PULSE_MS;
}

void gwStatus_setSessionArmed(bool on) { digitalWrite(LED_SESSION_PIN, on ? HIGH : LOW); }

void gwStatus_setFault(bool on) {
    _fault = on;
    digitalWrite(LED_FAULT_PIN, on ? HIGH : LOW);
}

void gwStatus_update() {
    if (_rs485PulseUntil && (long)(millis() - _rs485PulseUntil) >= 0) {
        digitalWrite(LED_RS485_PIN, LOW);
        _rs485PulseUntil = 0;
    }
}

// ── Boot health ────────────────────────────────────────────────────────────
uint8_t gwStatus_bootAttempts() { return _bootAttempts; }
bool    gwStatus_safeMode()     { return _safeMode; }

void gwStatus_setSafeMode(bool on) {
    _safeMode = on;
    if (on) gwStatus_setFault(true);
}

void gwStatus_markHealthy() {
    if (bkpRead(BKP_REG_BOOTS) != 0) bkpWrite(BKP_REG_BOOTS, 0);
}

const char* gwStatus_resetReason() { return _resetReason; }
uint8_t     gwStatus_resetReasonCode() { return _resetCode; }
uint32_t    gwStatus_uptimeS()     { return (millis() - _bootMs) / 1000UL; }
uint32_t    gwStatus_lastRs485Ms() { return _lastRs485Ms; }

// ── Identity ───────────────────────────────────────────────────────────────
const uint8_t* gwStatus_mac()  { return _mac; }
bool           gwStatus_macValid() { return _macValid; }

void gwStatus_setMac(const uint8_t* mac) {
    if (!mac || !macLooksReal(mac)) return;
    memcpy(_mac, mac, sizeof(_mac));
    _macValid = true;
}

void gwStatus_serialHex(char* out, size_t n) {
    uint8_t uid[64];
    uint8_t len = getUniqueSerialNumber(uid);
    size_t take = (len > 4) ? 4 : len;                 // 8 hex chars is plenty
    size_t pos = 0;
    for (size_t i = 0; i < take && pos + 3 < n; i++) {
        pos += snprintf(out + pos, n - pos, "%02X", uid[i]);
    }
    if (pos < n) out[pos] = '\0';
}

int gwStatus_buttonRaw() { return digitalRead(GW_BUTTON_PIN); }
