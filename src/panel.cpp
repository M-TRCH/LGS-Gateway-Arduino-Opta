#include "panel.h"
#include "modbus_rtu.h"
#include "gw_status.h"
#include "gw_store.h"
#include "net_runtime.h"

// Inputs 1-5 on the Opta terminal block. Red, green, blue, yellow, white in
// the order they are wired; what each one does is configuration.
static const pin_size_t PANEL_PIN[PANEL_BUTTONS] = { A0, A1, A2, A3, A4 };

static uint8_t  _action[PANEL_BUTTONS] = {0};
static uint8_t  _enabled = 0;
static uint16_t _cabinet = PANEL_CABINET_64;
static uint16_t _stepMs = 0;
static uint16_t _resetMs = 0;

// Idle level per input, sampled at boot. The buttons may be wired to apply
// voltage or to remove it, and a gateway that has to be told which is a
// gateway someone will get wrong — so "pressed" simply means "not what it
// looked like at power-up".
static uint8_t _idleLevel[PANEL_BUTTONS] = {0};
static uint8_t _stable[PANEL_BUTTONS] = {0};
static uint8_t _lastRead[PANEL_BUTTONS] = {0};
static uint32_t _lastChange[PANEL_BUTTONS] = {0};
static const uint32_t DEBOUNCE_MS = 40;

// Current sweep
static uint8_t  _running = PANEL_NONE;
static uint16_t _index = 0;
static uint16_t _total = 0;
static uint32_t _nextStepAt = 0;
static uint32_t _resetUntil = 0;

// Lamps
static uint8_t  _lamp = LAMP_RED;      // red until the first decision
static uint32_t _lampSince = 0;

// ── Cabinet shapes ─────────────────────────────────────────────────────────
// The number in an LGS name is the slot count. 40 and 80 are plain blocks;
// 64 is not — its middle rows are half width, so it is spelled out.
uint16_t panel_slotCount(uint16_t cabinet) {
    if (cabinet == PANEL_CABINET_40) return 40;
    if (cabinet == PANEL_CABINET_64) return 64;
    return 80;
}

uint8_t panel_slotAt(uint16_t cabinet, uint16_t index) {
    if (index >= panel_slotCount(cabinet)) return 0;
    if (cabinet == PANEL_CABINET_40) {
        return (uint8_t)(((index / 4) + 1) * 10 + (index % 4) + 1);
    }
    if (cabinet == PANEL_CABINET_64) {
        // rows 1-3 eight wide, rows 4-7 four wide, rows 8-10 eight wide
        if (index < 24) return (uint8_t)(((index / 8) + 1) * 10 + (index % 8) + 1);
        if (index < 40) {
            const uint16_t k = index - 24;
            return (uint8_t)(((k / 4) + 4) * 10 + (k % 4) + 1);
        }
        const uint16_t k = index - 40;
        return (uint8_t)(((k / 8) + 8) * 10 + (k % 8) + 1);
    }
    return (uint8_t)(((index / 8) + 1) * 10 + (index % 8) + 1);
}

// ── Modbus helpers ─────────────────────────────────────────────────────────
static void writeCoil(uint8_t slave, uint16_t coil, bool on) {
    uint8_t tx[8];
    tx[0] = slave;
    tx[1] = 0x05;
    tx[2] = (uint8_t)(coil >> 8);
    tx[3] = (uint8_t)(coil & 0xFF);
    tx[4] = on ? 0xFF : 0x00;
    tx[5] = 0x00;
    const uint16_t crc = crc16(tx, 6);
    tx[6] = (uint8_t)(crc & 0xFF);
    tx[7] = (uint8_t)(crc >> 8);
    uint8_t rx[RTU_BUF_SIZE];
    rtu_transact(tx, 8, rx);        // reply ignored: a sweep is not a survey
}

static void writeReg(uint8_t slave, uint16_t reg, uint16_t value) {
    uint8_t tx[8];
    tx[0] = slave;
    tx[1] = 0x06;
    tx[2] = (uint8_t)(reg >> 8);
    tx[3] = (uint8_t)(reg & 0xFF);
    tx[4] = (uint8_t)(value >> 8);
    tx[5] = (uint8_t)(value & 0xFF);
    const uint16_t crc = crc16(tx, 6);
    tx[6] = (uint8_t)(crc & 0xFF);
    tx[7] = (uint8_t)(crc >> 8);
    uint8_t rx[RTU_BUF_SIZE];
    rtu_transact(tx, 8, rx);
}

// What the OLED shows: the slave ID when it fits in two digits, otherwise
// the column, matching what the test tool does.
static uint16_t displayValue(uint8_t slave) {
    return slave <= 99 ? slave : (uint16_t)(slave % 10);
}

// ── Sweeps ─────────────────────────────────────────────────────────────────
static void startSweep(uint8_t action) {
    _running = action;
    _index = 0;
    _total = (action == PANEL_RESET) ? 0 : panel_slotCount(_cabinet);
    _nextStepAt = millis();
    if (action == PANEL_RESET) {
        // Restored by the tick once _resetUntil passes, so a reset that
        // starts is a reset that finishes even if the button is spammed.
        digitalWrite(MODULE_RELAY_PIN, LOW);
        _resetUntil = millis() + _resetMs;
    }
}

static void stepSweep() {
    const uint8_t slave = panel_slotAt(_cabinet, _index);
    if (slave == 0) { _running = PANEL_NONE; _total = 0; return; }

    switch (_running) {
        case PANEL_ALL_ON:
            writeReg(slave, 60, displayValue(slave));
            writeCoil(slave, 1011, true);
            break;
        case PANEL_ALL_UNLOCK:
            writeReg(slave, 60, displayValue(slave));
            writeCoil(slave, 1031, true);
            break;
        case PANEL_ALL_OFF:
            // 1011 is the state coil for ring + display, and clearing it
            // clears both — the same one the latch commands mirror, so this
            // also puts out whatever a blue press left lit.
            writeCoil(slave, 1011, false);
            break;
        default:
            break;
    }
    _index++;
    if (_index >= _total) { _running = PANEL_NONE; _total = 0; }
    _nextStepAt = millis() + _stepMs;
}

// ── Lifecycle ──────────────────────────────────────────────────────────────
void panel_applyConfig() {
    const GwConfig& c = gwConfig_active();
    _enabled = c.panel_enabled;
    _cabinet = c.panel_cabinet;
    _stepMs = c.panel_step_ms;
    _resetMs = c.panel_reset_ms;
    for (int i = 0; i < PANEL_BUTTONS; i++) {
        _action[i] = c.panel_btn[i] > PANEL_ACTION_MAX ? PANEL_NONE : c.panel_btn[i];
    }
}

// Which lamp the gateway's state calls for, worst news first.
static uint8_t lampWanted(uint32_t now) {
    if (_resetUntil) return LAMP_RED;
    if (gwStatus_safeMode() || !gwStore_available()) return LAMP_RED;
    if (gwConfig_active().net_enabled && !netRuntime_isUp()) return LAMP_RED;
    if (gwStatus_consecutiveTimeouts() >= PANEL_LAMP_DEAD_TIMEOUTS) return LAMP_RED;

    if (_running != PANEL_NONE) return LAMP_AMBER;
    const uint32_t last = gwStatus_lastRs485Ms();
    if (last && now - last < PANEL_LAMP_ACTIVITY_MS) return LAMP_AMBER;

    return LAMP_GREEN;
}

static void lampApply(uint8_t lamp) {
    digitalWrite(PANEL_LAMP_GREEN,  lamp == LAMP_GREEN  ? HIGH : LOW);
    digitalWrite(PANEL_LAMP_YELLOW, lamp == LAMP_AMBER  ? HIGH : LOW);
    digitalWrite(PANEL_LAMP_RED,    lamp == LAMP_RED    ? HIGH : LOW);
}

static void lampUpdate(uint32_t now) {
    const uint8_t want = lampWanted(now);
    if (want == _lamp) return;
    // Relays, not LEDs: never switch faster than the dwell.
    if (now - _lampSince < PANEL_LAMP_DWELL_MS) return;
    _lamp = want;
    _lampSince = now;
    lampApply(_lamp);
}

void panel_begin() {
    pinMode(PANEL_LAMP_GREEN,  OUTPUT);
    pinMode(PANEL_LAMP_YELLOW, OUTPUT);
    pinMode(PANEL_LAMP_RED,    OUTPUT);
    _lamp = LAMP_RED;               // until the first decision says otherwise
    _lampSince = millis();
    lampApply(_lamp);

    for (int i = 0; i < PANEL_BUTTONS; i++) {
        pinMode(PANEL_PIN[i], INPUT);
        const uint8_t level = (uint8_t)digitalRead(PANEL_PIN[i]);
        _idleLevel[i] = level;
        _stable[i] = level;
        _lastRead[i] = level;
        _lastChange[i] = millis();
    }
    panel_applyConfig();
}

void panel_update() {
    const uint32_t now = millis();

    // A reset outlives the press that started it: restore the rails on time
    // whatever else is going on.
    if (_resetUntil && now >= _resetUntil) {
        digitalWrite(MODULE_RELAY_PIN, HIGH);
        _resetUntil = 0;
        _running = PANEL_NONE;
    }

    if (_enabled) {
        for (int i = 0; i < PANEL_BUTTONS; i++) {
            const uint8_t level = (uint8_t)digitalRead(PANEL_PIN[i]);
            if (level != _lastRead[i]) {
                _lastRead[i] = level;
                _lastChange[i] = now;
                continue;
            }
            if (level == _stable[i] || now - _lastChange[i] < DEBOUNCE_MS) continue;
            _stable[i] = level;
            // Act on the press, never on the release: holding a button must
            // not run the sweep twice.
            if (level != _idleLevel[i] && _action[i] != PANEL_NONE) {
                startSweep(_action[i]);
            }
        }
    }

    // One slot per tick at most, so the bridges keep their share of the loop.
    if (_running != PANEL_NONE && _running != PANEL_RESET && now >= _nextStepAt) {
        stepSweep();
    }

    // The lamps report the gateway, not the buttons, so they run even when
    // the buttons are switched off.
    lampUpdate(now);
}

const char* panel_lampName() {
    switch (_lamp) {
        case LAMP_AMBER: return "amber";
        case LAMP_RED:   return "red";
        default:         return "green";
    }
}

const char* panel_actionName(uint8_t action) {
    switch (action) {
        case PANEL_ALL_ON:     return "all_on";
        case PANEL_ALL_OFF:    return "all_off";
        case PANEL_ALL_UNLOCK: return "all_unlock";
        case PANEL_RESET:      return "reset";
        default:               return "none";
    }
}

const char* panel_stateName() {
    return _running == PANEL_NONE ? "idle" : panel_actionName(_running);
}

uint16_t panel_progress() { return _index; }
uint16_t panel_total()    { return _total; }

uint8_t panel_inputMask() {
    uint8_t mask = 0;
    for (int i = 0; i < PANEL_BUTTONS; i++) {
        if (_stable[i] != _idleLevel[i]) mask |= (uint8_t)(1u << i);
    }
    return mask;
}
