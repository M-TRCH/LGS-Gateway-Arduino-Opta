#include "panel.h"
#include "event_log.h"
#include "modbus_rtu.h"
#include "gw_status.h"
#include "gw_store.h"
#include "net_runtime.h"
#include "tcp_bridge.h"

// Inputs 1-5 on the Opta terminal block. Red, green, blue, yellow, white in
// the order they are wired; what each one does is configuration.
static const pin_size_t PANEL_PIN[PANEL_BUTTONS] = { A0, A1, A2, A3, A4 };

static uint8_t  _action[PANEL_BUTTONS] = {0};
static uint8_t  _enabled = 0;
static uint16_t _cabinet = PANEL_CABINET_64;
// Slots per row, all-zero = follow _cabinet's preset. This is the cabinet
// that is not in the catalogue: the sweep walks whatever is really there.
static uint8_t  _shape[GW_SHAPE_ROWS] = {0};
// Pacing per sweep kind. The unlock pause is really a power budget: it
// spaces the solenoid firings out so a whole-cabinet unlock cannot ask the
// supply for many latches at once.
static uint16_t _stepOnMs = 0;
static uint16_t _stepOffMs = 0;
static uint16_t _stepUnlockMs = 0;
static uint16_t _resetMs = 0;
// Which module preset the sweeps fire. Brightness and colour live in the
// preset's per-module config — the panel picks the look, it does not own it.
static uint8_t  _preset = DEF_PANEL_PRESET;
// Test brightness: 0 = the preset's own; else the module's VOLATILE global
// brightness (reg 190) is written before lighting, so a test look cannot
// outlive a power cycle or rewrite anything a site configured.
static uint8_t  _bright = DEF_PANEL_BRIGHT;

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

// Lamps. Each of outputs 2-4 follows whatever it is mapped to; the three
// state sources (ready/busy/fault) are mutually exclusive, so mapping them to
// three outputs gives a traffic light, while the rest are plain facts.
static uint8_t  _outSrc[PANEL_OUTPUTS] = { DEF_PANEL_OUT1, DEF_PANEL_OUT2,
                                           DEF_PANEL_OUT3, DEF_PANEL_OUT4 };
static uint8_t  _outLit[PANEL_OUTPUTS] = {0};
static uint32_t _outSince[PANEL_OUTPUTS] = {0};
static uint8_t  _lampsOn = 1;
static uint16_t _lampHoldMs = DEF_PANEL_LAMP_HOLD_MS;
static uint16_t _lampDwellMs = DEF_PANEL_LAMP_DWELL_MS;
static uint16_t _lampDead = DEF_PANEL_LAMP_DEAD;
static void outWrite(uint8_t i, bool on);  // defined with the lamp logic
static uint8_t  _forceOut = 0xFF;       // 0xFF = outputs follow their mapping
static uint32_t _forceUntil = 0;

// Index 0..3 -> outputs 1..4 on the terminal block.
static int outPin(uint8_t i) {
    switch (i) {
        case 0:  return PANEL_OUT_1;
        case 1:  return PANEL_OUT_2;
        case 2:  return PANEL_OUT_3;
        default: return PANEL_OUT_4;
    }
}

// An output carrying the shelf's power is not a lamp: it is not rate-limited
// by the dwell, not switched off with the lamps, and not touched by a lamp
// test. Cutting the cabinet to check a bulb would be a poor trade.
static bool isShelf(uint8_t i) { return _outSrc[i] == SRC_SHELF; }

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

// The shape the sweeps actually walk: the custom shape when one is set,
// else the preset. Top row first, left to right, absent rows skipped.
static bool shapeSet() {
    for (int r = 0; r < GW_SHAPE_ROWS; r++) {
        if (_shape[r]) return true;
    }
    return false;
}

uint16_t panel_activeSlotCount() {
    if (!shapeSet()) return panel_slotCount(_cabinet);
    uint16_t n = 0;
    for (int r = 0; r < GW_SHAPE_ROWS; r++) n += _shape[r];
    return n;
}

uint8_t panel_activeSlotAt(uint16_t index) {
    if (!shapeSet()) return panel_slotAt(_cabinet, index);
    for (int r = 0; r < GW_SHAPE_ROWS; r++) {
        if (index < _shape[r]) return (uint8_t)((r + 1) * 10 + index + 1);
        index -= _shape[r];
    }
    return 0;
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
    _total = (action == PANEL_RESET) ? 0 : panel_activeSlotCount();
    _nextStepAt = millis();
    if (action == PANEL_RESET) {
        // Only the deadline is set here; whichever output is mapped to the
        // shelf's power follows it on the next tick and restores itself when
        // it passes, so a reset that starts is a reset that finishes even if
        // the button is spammed.
        _resetUntil = millis() + _resetMs;
    }
}

void panel_startReset() { startSweep(PANEL_RESET); }

static void stepSweep() {
    const uint8_t slave = panel_activeSlotAt(_index);
    if (slave == 0) { _running = PANEL_NONE; _total = 0; return; }

    // Coils 1010+p (ring + display) and 1030+p (plus the latch) for the
    // configured preset. The latch coil mirrors the display coil of the SAME
    // preset, so all_off clearing 1010+p also puts out what a blue press lit.
    const uint16_t coilDisplay = (uint16_t)(1010 + _preset);
    switch (_running) {
        case PANEL_ALL_ON:
            if (_bright) writeReg(slave, 190, _bright);
            writeReg(slave, 60, displayValue(slave));
            writeCoil(slave, coilDisplay, true);
            break;
        case PANEL_ALL_UNLOCK:
            if (_bright) writeReg(slave, 190, _bright);
            writeReg(slave, 60, displayValue(slave));
            writeCoil(slave, (uint16_t)(1030 + _preset), true);
            break;
        case PANEL_ALL_OFF:
            writeCoil(slave, coilDisplay, false);
            break;
        default:
            break;
    }
    _index++;
    if (_index >= _total) { _running = PANEL_NONE; _total = 0; }
    const uint16_t pace = (_running == PANEL_ALL_UNLOCK) ? _stepUnlockMs
                        : (_running == PANEL_ALL_OFF)    ? _stepOffMs
                        : _stepOnMs;
    _nextStepAt = millis() + pace;
}

// ── Lifecycle ──────────────────────────────────────────────────────────────
void panel_applyConfig() {
    const GwConfig& c = gwConfig_active();
    _enabled = c.panel_enabled;
    _cabinet = c.panel_cabinet;
    memcpy(_shape, c.panel_shape, sizeof(_shape));
    _preset = (c.panel_preset >= 1 && c.panel_preset <= 8)
              ? c.panel_preset : DEF_PANEL_PRESET;
    _bright = c.panel_bright <= 100 ? c.panel_bright : DEF_PANEL_BRIGHT;
    _stepOnMs = c.panel_step_on_ms;
    _stepOffMs = c.panel_step_off_ms;
    _stepUnlockMs = c.panel_step_unlock_ms;
    _resetMs = c.panel_reset_ms;
    _lampHoldMs = c.panel_lamp_hold_ms;
    _lampDwellMs = c.panel_lamp_dwell_ms;
    _lampDead = c.panel_lamp_dead;
    if (_lampsOn && !c.panel_lamps) {   // switched off: put the lamps out
        for (uint8_t i = 0; i < PANEL_OUTPUTS; i++) {
            if (!isShelf(i)) outWrite(i, false);
        }
    }
    _lampsOn = c.panel_lamps;
    for (uint8_t i = 0; i < PANEL_OUTPUTS; i++) {
        _outSrc[i] = c.panel_out[i] > SRC_MAX ? SRC_NONE : c.panel_out[i];
        _outSince[i] = 0;               // a remapped output may light at once
    }
    for (int i = 0; i < PANEL_BUTTONS; i++) {
        _action[i] = c.panel_btn[i] > PANEL_ACTION_MAX ? PANEL_NONE : c.panel_btn[i];
    }
}

// The gateway's one state, worst news first. Reported as three separate
// sources so a panel can show all three, or only the one it has a lamp for.
static uint8_t gatewayState(uint32_t now) {
    if (_resetUntil) return SRC_FAULT;
    if (gwStatus_safeMode() || !gwStore_available()) return SRC_FAULT;
    if (gwConfig_active().net_enabled && !netRuntime_isUp()) return SRC_FAULT;
    if (gwStatus_consecutiveTimeouts() >= _lampDead) return SRC_FAULT;
    if (_running != PANEL_NONE) return SRC_BUSY;
    const uint32_t last = gwStatus_lastRs485Ms();
    if (last && now - last < _lampHoldMs) return SRC_BUSY;
    return SRC_READY;
}

static bool sourceTrue(uint8_t src, uint8_t state) {
    switch (src) {
        case SRC_READY:  return state == SRC_READY;
        case SRC_BUSY:   return state == SRC_BUSY;
        case SRC_FAULT:  return state == SRC_FAULT;
        case SRC_LINK:   return netRuntime_isUp();
        case SRC_CLIENT: return tcpBridge_hasClient();
        case SRC_SWEEP:  return _running != PANEL_NONE;
        case SRC_RESET:  return _resetUntil != 0;
        // Energised except while a reset is running — dropping it IS the reset.
        case SRC_SHELF:  return _resetUntil == 0;
        default:         return false;      // SRC_NONE
    }
}

static void outWrite(uint8_t i, bool on) {
    _outLit[i] = on ? 1 : 0;
    digitalWrite(outPin(i), on ? HIGH : LOW);
}

static void lampUpdate(uint32_t now) {
    if (_forceOut != 0xFF) {            // a wiring check is in progress
        if ((int32_t)(now - _forceUntil) < 0) return;
        _forceOut = 0xFF;               // expired: hand the outputs back
        for (uint8_t i = 0; i < PANEL_OUTPUTS; i++) _outSince[i] = 0;
    }
    const uint8_t state = gatewayState(now);
    for (uint8_t i = 0; i < PANEL_OUTPUTS; i++) {
        // The shelf's power keeps running even when the lamps are switched
        // off — that switch is about lamps, not about the cabinet.
        if (!_lampsOn && !isShelf(i)) continue;
        const bool want = sourceTrue(_outSrc[i], state);
        if (want == (bool)_outLit[i]) continue;
        // Relays, not LEDs: never switch a LAMP faster than the dwell. The
        // shelf's power is exempt — a reset must start and end on time.
        if (!isShelf(i) && _outSince[i] && now - _outSince[i] < _lampDwellMs) continue;
        _outSince[i] = now;
        outWrite(i, want);
    }
}

void panel_begin() {
    for (uint8_t i = 0; i < PANEL_OUTPUTS; i++) {
        pinMode(outPin(i), OUTPUT);
        // Lamps dark until the first decision; the shelf powered from the
        // start, because a gateway booting must not cut the cabinet.
        outWrite(i, isShelf(i));
        _outSince[i] = 0;
    }

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
        _resetUntil = 0;                // the shelf output follows below
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
                // Manual actions only — a scheduled reset logs its own event.
                eventLog_note(GW_EV_SWEEP, _action[i], (uint16_t)(i + 1));
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

void panel_forceLamp(uint8_t out, uint32_t ms) {
    _forceOut = out;
    _forceUntil = millis() + ms;
    for (uint8_t i = 0; i < PANEL_OUTPUTS; i++) {
        if (isShelf(i)) continue;       // never cut the cabinet to test a lamp
        outWrite(i, out != PANEL_LAMP_OFF && (uint8_t)(i + 1) == out);
    }
}

const char* panel_sourceName(uint8_t source) {
    switch (source) {
        case SRC_READY:  return "ready";
        case SRC_BUSY:   return "busy";
        case SRC_FAULT:  return "fault";
        case SRC_LINK:   return "link";
        case SRC_CLIENT: return "client";
        case SRC_SWEEP:  return "sweep";
        case SRC_RESET:  return "reset";
        case SRC_SHELF:  return "shelf";
        default:         return "none";
    }
}

// "234" with a dash for each output that is dark, so one glance at INFO says
// what the panel is showing: "-3-" is the middle lamp only.
const char* panel_lampName() {
    static char buf[8];
    if (!_lampsOn) return "off";
    for (uint8_t i = 0; i < PANEL_OUTPUTS; i++) {
        buf[i] = _outLit[i] ? (char)('1' + i) : '-';
    }
    buf[PANEL_OUTPUTS] = '\0';
    if (_forceOut != 0xFF) {
        static char forced[16];
        snprintf(forced, sizeof(forced), "forced:%s", buf);
        return forced;
    }
    return buf;
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
