#include "mod_sweep.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "config.h"
#include "gw_config.h"
#include "gw_console.h"
#include "gw_status.h"
#include "modbus_rtu.h"
#include "panel.h"

#define SW_MAX      80
#define SW_SLOW_MS  150

enum SwSt : uint8_t { SW_TODO = 0, SW_OK = 1, SW_SLOW = 2, SW_SILENT = 3 };
struct SwRow { uint8_t id, st, tries; uint16_t ms, fw, type; };

static SwRow   _rows[SW_MAX];
static uint8_t _n = 0, _pos = 0;
static bool    _run = false;
static uint32_t _startMs = 0, _endMs = 0;

bool modSweep_running() { return _run; }

static bool readTwo(uint8_t id, uint16_t& r0, uint16_t& r1, uint16_t& ms) {
    uint8_t tx[8] = {id, 0x03, 0x00, 0x00, 0x00, 0x02, 0, 0};
    const uint16_t c = crc16(tx, 6);
    tx[6] = (uint8_t)(c & 0xFF);
    tx[7] = (uint8_t)(c >> 8);
    uint8_t rx[RTU_BUF_SIZE];
    const uint32_t t0 = millis();
    const int n = rtu_transact(tx, 8, rx, 0);
    ms = (uint16_t)min(millis() - t0, 65535UL);
    gwStatus_pulseRs485();
    if (n < 9 || rx[0] != id || rx[1] != 0x03 || rx[2] != 4 || !verifyCRC(rx, n)) return false;
    r0 = ((uint16_t)rx[3] << 8) | rx[4];
    r1 = ((uint16_t)rx[5] << 8) | rx[6];
    return true;
}

void modSweep_update() {
    if (!_run) return;
    if (_pos >= _n) { _run = false; _endMs = millis(); return; }
    SwRow& r = _rows[_pos];
    uint16_t a = 0, b = 0, ms = 0;
    r.tries++;
    if (readTwo(r.id, a, b, ms)) {
        r.type = a; r.fw = b; r.ms = ms;
        r.st = ms > SW_SLOW_MS ? SW_SLOW : SW_OK;
        _pos++;
    } else if (r.tries >= 2) {
        r.st = SW_SILENT; r.ms = ms;
        _pos++;
    }
    gwStatus_watchdogKick();
}

static uint8_t buildIds(uint8_t* ids) {
    uint8_t n = 0;
    const uint16_t cnt = panel_activeSlotCount();
    for (uint16_t i = 0; i < cnt && n < SW_MAX; i++) ids[n++] = panel_activeSlotAt(i);
    if (n) return n;
    const GwConfig& c = gwConfig_active();
    for (int row = 1; row <= GW_HUB_MAX_ROWS && n < SW_MAX; row++) {
        if (!c.hub_map[row - 1]) continue;
        for (int col = 1; col <= 8 && n < SW_MAX; col++) ids[n++] = (uint8_t)(row * 10 + col);
    }
    if (n) return n;
    for (int row = 1; row <= 10; row++)
        for (int col = 1; col <= 8; col++) ids[n++] = (uint8_t)(row * 10 + col);
    return n;
}

static bool parseIds(const char* s, uint8_t* ids, uint8_t& n) {
    n = 0;
    while (*s) {
        char* e;
        long a = strtol(s, &e, 10);
        if (e == s || a < 1 || a > 247) return false;
        long b = a;
        if (*e == '-') { s = e + 1; b = strtol(s, &e, 10); if (e == s || b < a || b > 247) return false; }
        for (long v = a; v <= b; v++) { if (n >= SW_MAX) return false; ids[n++] = (uint8_t)v; }
        if (*e == ',') s = e + 1; else if (*e == 0) s = e; else return false;
    }
    return n > 0;
}

static const char* stName(uint8_t s) {
    switch (s) { case SW_OK: return "ok"; case SW_SLOW: return "slow"; case SW_SILENT: return "silent"; default: return "todo"; }
}

static char _ln[128];
static void out(void (*emit)(const char*), const char* fmt, ...) __attribute__((format(printf, 2, 3)));
static void out(void (*emit)(const char*), const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(_ln, sizeof(_ln), fmt, ap);
    va_end(ap);
    emit(_ln);
}

// MOD <id> LIGHT|UNLOCK|OFF: one command to one module, for the service page and a technician's laptop. LIGHT lights the
// ring and number of the panel preset (coil 1010+p, stays lit until OFF), UNLOCK is the unlock combo (1030+p, the latch
// pulses and clears itself), OFF is the module's own all-off (511). Needs HELLO like every other write, never runs while
// a SWEEP walks the bus, and is sent without the heal retry (this path is also reachable from the USB console).
void modCommand(char** argv, int argc, void (*emit)(const char*)) {
    if (!gwConsole_armed()) { emit("#ERR MOD err=locked hint=HELLO"); return; }
    if (argc != 2) { emit("#ERR MOD err=syntax want=<id>_LIGHT|UNLOCK|OFF"); return; }
    const long id = strtol(argv[0], nullptr, 10);
    if (id < 11 || id > 108 || id % 10 < 1 || id % 10 > 8) { emit("#ERR MOD err=range key=id allowed=11-108"); return; }
    if (_run) { emit("#ERR MOD err=busy detail=sweep_running"); return; }
    const uint16_t p = gwConfig_active().panel_preset >= 1 && gwConfig_active().panel_preset <= 8 ? gwConfig_active().panel_preset : 1;
    uint16_t coil;
    if (!strcasecmp(argv[1], "LIGHT")) coil = 1010 + p;
    else if (!strcasecmp(argv[1], "UNLOCK")) coil = 1030 + p;
    else if (!strcasecmp(argv[1], "OFF")) coil = 511;
    else { emit("#ERR MOD err=syntax want=<id>_LIGHT|UNLOCK|OFF"); return; }
    uint8_t tx[8] = {(uint8_t)id, 0x05, (uint8_t)(coil >> 8), (uint8_t)(coil & 0xFF), 0xFF, 0x00, 0, 0};
    const uint16_t c = crc16(tx, 6);
    tx[6] = (uint8_t)(c & 0xFF);
    tx[7] = (uint8_t)(c >> 8);
    uint8_t rx[RTU_BUF_SIZE];
    const int n = rtu_transact(tx, 8, rx, 0);
    gwStatus_pulseRs485();
    const bool ok = n >= 8 && rx[0] == id && rx[1] == 0x05 && verifyCRC(rx, n);
    if (!ok) { out(emit, "#ERR MOD err=no_reply id=%ld", id); return; }
    out(emit, "#OK MOD id=%ld %s coil=%u", id, argv[1], (unsigned)coil);
}

void modSweep_command(char** argv, int argc, void (*emit)(const char*)) {
    const char* sub = argc > 0 ? argv[0] : "STATUS";
    if (!strcasecmp(sub, "START")) {
        uint8_t ids[SW_MAX], n = 0;
        const char* v = nullptr;
        for (int i = 1; i < argc; i++) if (!strncmp(argv[i], "ids=", 4)) v = argv[i] + 4;
        if (v && strcmp(v, "all") != 0) {
            if (!parseIds(v, ids, n)) { emit("#ERR SWEEP err=syntax want=ids=11-18,21|all"); return; }
        } else {
            n = buildIds(ids);
        }
        for (int i = 0; i < n; i++) _rows[i] = SwRow{ids[i], SW_TODO, 0, 0, 0, 0};
        _n = n; _pos = 0; _run = true; _startMs = millis(); _endMs = 0;
        out(emit, "#OK SWEEP state=running total=%u", n);
        return;
    }
    if (!strcasecmp(sub, "STOP")) { _run = false; _endMs = millis(); emit("#OK SWEEP state=stopped"); return; }
    if (!strcasecmp(sub, "STATUS")) {
        int ok = 0, slow = 0, silent = 0;
        for (int i = 0; i < _pos && i < _n; i++) { ok += _rows[i].st == SW_OK; slow += _rows[i].st == SW_SLOW; silent += _rows[i].st == SW_SILENT; }
        const uint32_t ms = _startMs ? ((_run ? millis() : _endMs) - _startMs) : 0;
        out(emit, "#DATA state=%s done=%u total=%u ok=%d slow=%d silent=%d ms=%lu", _run ? "running" : (_n ? "done" : "idle"),
            _pos, _n, ok, slow, silent, (unsigned long)ms);
        for (int i = 0; i < _pos && i < _n; i++)
            out(emit, "#DATA id=%u st=%s ms=%u fw=%u ty=%u", _rows[i].id, stName(_rows[i].st), _rows[i].ms, _rows[i].fw, _rows[i].type);
        out(emit, "#OK SWEEP n=%u", _pos);
        return;
    }
    emit("#ERR SWEEP err=syntax want=START|STATUS|STOP");
}
