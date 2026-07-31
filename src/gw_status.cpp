#include "gw_status.h"

#include <stm32h7xx_hal.h>

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
static unsigned long _rs485PulseUntil;

// ── RTC backup domain ──────────────────────────────────────────────────────
static uint32_t bkpRead(uint32_t reg) {
    return HAL_RTCEx_BKUPRead(&RTCHandle, reg);
}

static void bkpWrite(uint32_t reg, uint32_t value) {
    HAL_PWR_EnableBkUpAccess();
    HAL_RTCEx_BKUPWrite(&RTCHandle, reg, value);
}

static void latchResetReason() {
    if      (__HAL_RCC_GET_FLAG(RCC_FLAG_IWDG1RST)) _resetReason = "watchdog";
    else if (__HAL_RCC_GET_FLAG(RCC_FLAG_WWDG1RST)) _resetReason = "window-watchdog";
    else if (__HAL_RCC_GET_FLAG(RCC_FLAG_SFTRST))   _resetReason = "software";
    else if (__HAL_RCC_GET_FLAG(RCC_FLAG_PINRST))   _resetReason = "pin";
    else if (__HAL_RCC_GET_FLAG(RCC_FLAG_BORRST))   _resetReason = "brownout";
    else if (__HAL_RCC_GET_FLAG(RCC_FLAG_PORRST))   _resetReason = "power-on";
    __HAL_RCC_CLEAR_RESET_FLAGS();
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
    if (_getSecureEthMac_(_mac) != 1) {
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

void gwStatus_countRtu(bool replied, uint32_t rttMs) {
    if (rttMs > 0xFFFF) rttMs = 0xFFFF;
    _lastRtt = (uint16_t)rttMs;
    if (_lastRtt > _maxRtt) _maxRtt = _lastRtt;
    if (replied) {
        _counters[GW_RS485_OK]++;
        _consecTimeouts = 0;
        digitalWrite(LED_TIMEOUT_PIN, LOW);
    } else {
        _counters[GW_RS485_TIMEOUT]++;
        if (_consecTimeouts < 0xFFFF) _consecTimeouts++;
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
    _rs485PulseUntil = millis() + RS485_PULSE_MS;
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
uint32_t    gwStatus_uptimeS()     { return (millis() - _bootMs) / 1000UL; }

// ── Identity ───────────────────────────────────────────────────────────────
const uint8_t* gwStatus_mac() { return _mac; }

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
