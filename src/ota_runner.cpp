#include "ota_runner.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <strings.h>
#include <string.h>

#include "config.h"
#include "event_log.h"
#include "gw_config.h"
#include "gw_remote.h"
#include "gw_status.h"
#include "modbus_rtu.h"
#include "panel.h"
#include "sched.h"
#include "usb_bridge.h"

// ── Module wire contract (LGS-Standard-Module include/svc/modbus_map.h; mirrors the Test Tool's ota.py) ──
#define REG_STATE        282
#define REG_META_FIRST   284
#define REG_CHUNK_FIRST  290            // idx, len, crc16, 64 data regs, commit counter = 68 regs
#define REG_BITMAP_FIRST 360
#define BITMAP_REGS      30
#define COIL_ENTER       505
#define COIL_FINALIZE    506
#define COIL_APPLY       507
#define COIL_ABORT       508
#define CHUNK_SIZE       128
#define MAX_IMAGE        61440UL
#define MAX_CHUNKS       480
#define MAX_MODS         80
#define MAX_GROUP        8

// ── Defaults (all overridable on START) ────────────────────────────────────
#define DEF_IDLE_S        5             // bus quiet before the runner transmits
#define DEF_GROUP         8
#define DEF_APPLY_IDLE_S  5             // bus quiet before a module is rebooted (it only matters to THAT module, which apply_quiet guards)
#define DEF_APPLY_QUIET_S 300           // nobody wrote to the module for this long
#define RESYNC_EVERY      8             // the proven pacing: a pause every 8 chunks ...
#define RESYNC_PAUSE_MS   600
#define SLOW_EVERY        2             // ... and the slow retry for modules that left the session
#define SLOW_PAUSE_MS     1000
#define KEEPALIVE_MS      8000
#define ENTER_ERASE_MS    1300          // the module erases its staging area (~0.7 s) before it listens again
#define REPAIR_ROUNDS     5
#define MAX_TRIES         6             // a module that keeps leaving the session is given up on after this many
#define FINALIZE_MS       1000
#define APPLY_WAIT_MS     6000          // reset + bootloader copy
#define APPLY_WINDOW_MS   8000          // a module is rebooted only if nobody is expected to ask for it within this long
#define CONFIRM_TRIES     5

// ── Per-module state ───────────────────────────────────────────────────────
enum ModState : uint8_t {
    M_PENDING = 0, M_NO_REPLY = 1, M_SESSION = 2, M_COMPLETE = 3, M_VERIFIED = 4,
    M_UPDATED = 5, M_SAME = 6, M_FAILED = 7, M_GAVE_UP = 8
};
static const char* modStateName(uint8_t s) {
    static const char* n[] = {"pending", "no_reply", "in_session", "complete", "verified",
                              "updated", "same_version", "failed", "gave_up"};
    return s < sizeof(n) / sizeof(n[0]) ? n[s] : "?";
}
// err: the module's own OTA error code (1..8) from register 282, or one of ours
#define ERR_NONE          0
#define ERR_NO_REPLY      100
#define ERR_NO_REPLY_POST 101
#define ERR_GAVE_UP       102
#define ERR_NOT_ENTERED   103

struct Mod {
    uint8_t  id, ch, st, err, tries;
    uint16_t fwBefore, fwAfter;
};
static Mod     _mods[MAX_MODS];
static uint8_t _nMods = 0;

// ── Run state ──────────────────────────────────────────────────────────────
enum Run : uint8_t { RUN_IDLE = 0, RUN_ACTIVE = 1 };
enum Phase : uint8_t {
    PH_PICK, PH_PROBE, PH_META, PH_ENTER, PH_ENTER_WAIT, PH_ENTER_CHECK, PH_STREAM, PH_REPAIR,
    PH_REPAIR_SEND, PH_FINALIZE, PH_FINALIZE_WAIT, PH_FINALIZE_CHECK, PH_APPLY_SEND, PH_APPLY_WAIT,
    PH_APPLY_CONFIRM, PH_WAIT_APPLY
};
static const char* phaseName(uint8_t p) {
    static const char* n[] = {"pick", "probe", "metadata", "enter", "enter_wait", "enter_check", "stream", "repair",
                              "repair_send", "finalize", "finalize_wait", "finalize_check", "apply", "apply_wait",
                              "apply_confirm", "waiting_for_quiet"};
    return p < sizeof(n) / sizeof(n[0]) ? n[p] : "?";
}

enum RunResult : uint8_t { RES_NONE = 0, RES_RUNNING = 1, RES_OK = 2, RES_FAILED = 3, RES_ABORTED = 4, RES_INTERRUPTED = 5 };
static const char* resName(uint8_t s) {
    static const char* n[] = {"none", "running", "done_ok", "done_with_failures", "aborted", "interrupted"};
    return s < sizeof(n) / sizeof(n[0]) ? n[s] : "?";
}

static Run      _run = RUN_IDLE;
static Phase    _ph = PH_PICK;
static bool     _abortReq = false;
static uint32_t _idleMs = DEF_IDLE_S * 1000UL, _applyIdleMs = DEF_APPLY_IDLE_S * 1000UL, _applyQuietMs = DEF_APPLY_QUIET_S * 1000UL;
static uint8_t  _groupMax = DEF_GROUP;
static uint32_t _waitUntil = 0;
static uint32_t _pausedSince = 0;        // millis() when the runner first had to hold back; 0 = not holding
static uint32_t _pausedTotalMs = 0;

// the image
static uint8_t  _image[MAX_IMAGE];
static uint32_t _imgSize = 0, _imgCrc = 0;
static uint16_t _total = 0;
static char     _imgName[25];
static uint8_t  _imgNameLen = 0;
static uint8_t  _slot = 0;

// the group in session
static uint8_t  _grp[MAX_GROUP];
static uint8_t  _grpN = 0, _grpCh = 0;
static bool     _slow = false;
static uint16_t _idx = 0;                // next chunk to stream, or position in the repair list
static uint16_t _sent = 0, _txCounter = 0;
static uint32_t _lastTx = 0;
static uint8_t  _pos = 0;                // member being worked on in a per-member phase
static uint8_t  _round = 0, _sub = 0;
static uint8_t  _missing[MAX_CHUNKS / 8];
static uint16_t _repairList[MAX_CHUNKS];
static uint16_t _repairN = 0;
static uint8_t  _applyIdx = 0;
static uint8_t  _confirmTry = 0;
static uint32_t _restarts = 0;

// the record
static uint32_t _seq = 0, _startEpoch = 0, _endEpoch = 0;

// ── Night window (OTA START at=HHMM until=HHMM [nights=N]) ──────────────────
// The cabinet is used all day; a module can only be rebooted when nobody is about to ask for it, and a pharmacy is busy
// enough that "when it is quiet" may never come. A window says WHEN the runner may work: it starts new groups and applies
// modules only inside [at, until) of the gateway's wall clock (which may wrap midnight), never within WIN_RESET_AFTER_S of
// a scheduled shelf reset (the reset power-cycles every module and ends a staging session) and never closer than
// WIN_BUDGET_S to the window's end or to the next reset (a group takes minutes and is not left half done). A group already
// in flight is finished. After `nights` windows have closed the run ends, with whatever is left recorded as such; ABORT
// ends it at any time. The run itself lives in RAM, so a gateway reboot ends it as before (the record says interrupted).
#define WIN_BUDGET_S      600UL
#define WIN_RESET_AFTER_S 180UL
#define WIN_MAX_NIGHTS    14
static bool     _winOn = false, _winPrev = false;
static uint32_t _winAtS = 0, _winUntilS = 0;      // seconds after midnight
static uint8_t  _nights = 1, _nightsDone = 0;

static uint32_t secOfDay() { return sched_now() % 86400UL; }

// Is the wall clock inside the window? (Time only: a reset hold-off is a separate question.)
static bool inWindow() {
    if (!_winOn) return true;
    const uint32_t s = secOfDay();
    return _winAtS < _winUntilS ? (s >= _winAtS && s < _winUntilS) : (s >= _winAtS || s < _winUntilS);
}

static uint32_t windowLeftS() {               // seconds until the window closes; meaningful only inside it
    const uint32_t s = secOfDay();
    return (_winUntilS + 86400UL - s) % 86400UL;
}

// May a module be rebooted now? Inside the window and clear of a shelf reset.
static bool windowOpen() {
    return !_winOn || (inWindow() && !sched_resetNear(60, WIN_RESET_AFTER_S));
}

// Every window the run asked for has opened and closed.
static bool windowsExhausted() { return _winOn && _nightsDone >= _nights && !inWindow(); }

// May a new group be started now? Room enough left in the window and before the next reset.
static bool groupMayStart() {
    return !_winOn || (inWindow() && windowLeftS() >= WIN_BUDGET_S && !sched_resetNear(WIN_BUDGET_S, WIN_RESET_AFTER_S));
}
static uint8_t  _result = RES_NONE;
static uint8_t  _lastWriteOk = 1;

static uint32_t now_() { return millis(); }
static bool     due(uint32_t t) { return (int32_t)(millis() - t) >= 0; }

// ── RS485 helpers: the runner's own frames, never counted as foreign traffic ─
static uint8_t _tx[RTU_BUF_SIZE], _rx[RTU_BUF_SIZE];

static void put16(uint8_t* p, uint16_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
static void seal(int n) {
    const uint16_t c = crc16(_tx, n);
    _tx[n] = (uint8_t)(c & 0xFF);
    _tx[n + 1] = (uint8_t)(c >> 8);
}
// The runner is the only caller that sets this: everything it puts on the bus must not make it look like a foreign request.
struct OwnTraffic {
    OwnTraffic() { rtu_setOtaCaller(true); }
    ~OwnTraffic() { rtu_setOtaCaller(false); }
};

static bool readRegs(uint8_t id, uint16_t addr, uint8_t n, uint16_t* out) {
    OwnTraffic own;
    _tx[0] = id; _tx[1] = 0x03; put16(_tx + 2, addr); put16(_tx + 4, n);
    seal(6);
    const int len = rtu_transact(_tx, 8, _rx, 0);
    if (len < 5 + 2 * n || _rx[0] != id || _rx[1] != 0x03 || _rx[2] != 2 * n || !verifyCRC(_rx, len)) return false;
    for (int i = 0; i < n; i++) out[i] = ((uint16_t)_rx[3 + 2 * i] << 8) | _rx[4 + 2 * i];
    return true;
}
static void sendBroadcastRegs(uint16_t addr, const uint16_t* v, uint8_t n) {
    OwnTraffic own;
    _tx[0] = 0; _tx[1] = 0x10; put16(_tx + 2, addr); put16(_tx + 4, n); _tx[6] = (uint8_t)(2 * n);
    for (int i = 0; i < n; i++) put16(_tx + 7 + 2 * i, v[i]);
    const int len = 7 + 2 * n;
    seal(len);
    rtu_send(_tx, len + 2);
}
static void sendBroadcastCoil(uint16_t coil) {
    OwnTraffic own;
    _tx[0] = 0; _tx[1] = 0x05; put16(_tx + 2, coil); _tx[4] = 0xFF; _tx[5] = 0x00;
    seal(6);
    rtu_send(_tx, 8);
}
// Unicast coil write. The module may reset or erase before it answers, so a missing reply is not an error.
static void writeCoil(uint8_t id, uint16_t coil) {
    OwnTraffic own;
    _tx[0] = id; _tx[1] = 0x05; put16(_tx + 2, coil); _tx[4] = 0xFF; _tx[5] = 0x00;
    seal(6);
    rtu_transact(_tx, 8, _rx, 0);
}

static uint16_t crc16ccitt(const uint8_t* p, int n) {
    uint16_t crc = 0xFFFF;
    while (n--) {
        crc ^= (uint16_t)(*p++) << 8;
        for (int i = 0; i < 8; i++) crc = (crc & 0x8000) ? (uint16_t)((crc << 1) ^ 0x1021) : (uint16_t)(crc << 1);
    }
    return crc;
}

static void sendChunk(uint16_t idx) {
    const uint32_t off = (uint32_t)idx * CHUNK_SIZE;
    uint16_t len = (uint16_t)(_imgSize - off < CHUNK_SIZE ? _imgSize - off : CHUNK_SIZE);
    uint16_t regs[68];
    regs[0] = idx;
    regs[1] = len;
    regs[2] = crc16ccitt(_image + off, len);
    for (int i = 0; i < 64; i++) {
        const uint16_t hi = (uint32_t)(2 * i) < len ? _image[off + 2 * i] : 0xFF;
        const uint16_t lo = (uint32_t)(2 * i + 1) < len ? _image[off + 2 * i + 1] : 0xFF;
        regs[3 + i] = (uint16_t)((hi << 8) | lo);
    }
    regs[67] = ++_txCounter;
    sendBroadcastRegs(REG_CHUNK_FIRST, regs, 68);
    _lastTx = now_();
    _sent++;
}

// ── The bus rule ───────────────────────────────────────────────────────────
// A step that touches the bus asks first. When the answer is no, the runner has paused: it remembers since when, and the
// pause is part of the status. Nothing here ever waits for the bus; the next loop() pass simply asks again.
static bool mayTransmit(uint32_t quietMs) {
    if (rtu_msSinceForeign() >= quietMs) {
        if (_pausedSince) { _pausedTotalMs += now_() - _pausedSince; _pausedSince = 0; }
        return true;
    }
    if (!_pausedSince) _pausedSince = now_();
    return false;
}

// Broadcast frames go out on whichever hub channel the hub was last left on, and any request from the bridges moves it. So
// before a broadcast the runner makes sure the hub is parked on its own group's channel and has settled.
static bool parked() {
    if (_grpCh == 0) return true;                      // no hub wiring: one bus
    if (rtu_hubLastChannel() == _grpCh) return rtu_hubSettled();
    uint16_t dummy[1];
    readRegs(_mods[_grp[0]].id, 0, 1, dummy);          // the crossing frame (lost by design) moves the hub
    return false;                                      // settles by itself; asked again next pass
}

// ── The record: two files on the OTA FAT partition, alternating, so the two latest runs survive ─────
struct __attribute__((packed)) ResHdr {
    char     magic[4];
    uint8_t  ver, state, flags, count;
    uint32_t seq, startEpoch, endEpoch, imgCrc, imgSize;
    uint8_t  nameLen;
    char     name[24];
    uint8_t  pad[3];
};
struct __attribute__((packed)) ResMod {
    uint8_t  id, st, err, tries;
    uint16_t fwBefore, fwAfter;
};
static_assert(sizeof(ResHdr) == 56, "result header layout");
static_assert(sizeof(ResMod) == 8, "result row layout");

static const char* resPath(int n) { return n == 0 ? "/fs/OTARES0.BIN" : "/fs/OTARES1.BIN"; }

static bool writeRecord(uint8_t state) {
    if (!gwRemote_fsMount()) { _lastWriteOk = 0; return false; }
    ResHdr h;
    memset(&h, 0, sizeof(h));
    memcpy(h.magic, "LGOR", 4);
    h.ver = 1; h.state = state; h.count = _nMods;
    h.seq = _seq; h.startEpoch = _startEpoch; h.endEpoch = _endEpoch; h.imgCrc = _imgCrc; h.imgSize = _imgSize;
    h.nameLen = _imgNameLen;
    memcpy(h.name, _imgName, _imgNameLen);
    FILE* f = fopen(resPath(_seq & 1), "wb");
    if (!f) { _lastWriteOk = 0; return false; }
    uint32_t crc = 0;
    bool ok = fwrite(&h, 1, sizeof(h), f) == sizeof(h);
    crc = gwRemote_crc32(crc, (const uint8_t*)&h, sizeof(h));
    for (int i = 0; ok && i < _nMods; i++) {
        ResMod r = {_mods[i].id, _mods[i].st, _mods[i].err, _mods[i].tries, _mods[i].fwBefore, _mods[i].fwAfter};
        ok = fwrite(&r, 1, sizeof(r), f) == sizeof(r);
        crc = gwRemote_crc32(crc, (const uint8_t*)&r, sizeof(r));
    }
    if (ok) ok = fwrite(&crc, 1, 4, f) == 4;
    fclose(f);
    _lastWriteOk = ok ? 1 : 0;
    return ok;
}

// Reads run `n` back (0 = latest, 1 = the one before). Returns false when there is no such record.
static bool readRecord(int n, ResHdr& h, ResMod* rows) {
    if (!gwRemote_fsMount()) return false;
    struct One { bool ok; ResHdr h; } rec[2];
    for (int i = 0; i < 2; i++) {
        rec[i].ok = false;
        FILE* f = fopen(resPath(i), "rb");
        if (!f) continue;
        ResHdr hh;
        if (fread(&hh, 1, sizeof(hh), f) == sizeof(hh) && memcmp(hh.magic, "LGOR", 4) == 0 && hh.count <= MAX_MODS) {
            uint32_t crc = gwRemote_crc32(0, (const uint8_t*)&hh, sizeof(hh));
            bool ok = true;
            for (int k = 0; ok && k < hh.count; k++) {
                ResMod r;
                ok = fread(&r, 1, sizeof(r), f) == sizeof(r);
                crc = gwRemote_crc32(crc, (const uint8_t*)&r, sizeof(r));
            }
            uint32_t want = 0;
            if (ok && fread(&want, 1, 4, f) == 4 && want == crc) { rec[i].ok = true; rec[i].h = hh; }
        }
        fclose(f);
    }
    int order[2], cnt = 0;                       // newest first
    if (rec[0].ok && rec[1].ok) { order[0] = rec[0].h.seq > rec[1].h.seq ? 0 : 1; order[1] = 1 - order[0]; cnt = 2; }
    else if (rec[0].ok) { order[0] = 0; cnt = 1; }
    else if (rec[1].ok) { order[0] = 1; cnt = 1; }
    if (n >= cnt) return false;
    const int which = order[n];
    FILE* f = fopen(resPath(which), "rb");
    if (!f) return false;
    fread(&h, 1, sizeof(h), f);
    if (rows) fread(rows, sizeof(ResMod), h.count, f);
    fclose(f);
    return true;
}

static uint32_t nextSeq() {
    ResHdr h;
    return readRecord(0, h, nullptr) ? h.seq + 1 : 1;
}

// ── Helpers ────────────────────────────────────────────────────────────────
static uint8_t channelOf(uint8_t id) {
    const int row = id / 10;
    if (row < 1 || row > GW_HUB_MAX_ROWS) return 0;
    return gwConfig_active().hub_map[row - 1];
}

static int countState(uint8_t st) {
    int n = 0;
    for (int i = 0; i < _nMods; i++) n += _mods[i].st == st;
    return n;
}
static int countWaitingApply() { return countState(M_VERIFIED); }
static int countPending() { return countState(M_PENDING); }
static int countFailed() { return countState(M_FAILED) + countState(M_GAVE_UP) + countState(M_NO_REPLY); }
static int countApplied() { return countState(M_UPDATED) + countState(M_SAME); }

static void note(uint8_t aux, uint16_t param) { eventLog_note(GW_EV_OTA, aux, param); }

static void finish(uint8_t state, uint8_t auxEvent) {
    _result = state;
    _endEpoch = sched_now();
    writeRecord(state);
    gwRemote_fsHold(false);
    note(auxEvent, (uint16_t)countFailed());
    _run = RUN_IDLE;
}

// Pick the next group of pending modules: same channel, same pacing, at most _groupMax of them. A channel the hub is already
// parked on is preferred: no extra crossing for whoever is using the cabinet.
static bool pickGroup() {
    int first = -1;
    const uint8_t cur = rtu_hubLastChannel();
    for (int i = 0; i < _nMods && first < 0; i++)
        if (_mods[i].st == M_PENDING && _mods[i].ch == cur) first = i;
    for (int i = 0; i < _nMods && first < 0; i++)
        if (_mods[i].st == M_PENDING) first = i;
    if (first < 0) return false;
    _grpCh = _mods[first].ch;
    _slow = _mods[first].tries > 0;
    _grpN = 0;
    for (int i = first; i < _nMods && _grpN < _groupMax && _grpN < MAX_GROUP; i++)
        if (_mods[i].st == M_PENDING && _mods[i].ch == _grpCh && (_mods[i].tries > 0) == _slow) _grp[_grpN++] = (uint8_t)i;
    return _grpN > 0;
}

static uint8_t resyncEvery() { return _slow ? SLOW_EVERY : RESYNC_EVERY; }
static uint32_t resyncPause() { return _slow ? SLOW_PAUSE_MS : RESYNC_PAUSE_MS; }

// A module left the session (or never joined): count the try, retry it slow, give up after MAX_TRIES.
static void dropMember(uint8_t gi, uint8_t err) {
    Mod& m = _mods[_grp[gi]];
    m.tries++;
    m.err = err;
    if (m.tries >= MAX_TRIES) { m.st = M_GAVE_UP; m.err = ERR_GAVE_UP; }
    else m.st = M_PENDING;
    _restarts++;
    note(3, m.id);
}

static void groupDone() {
    int verified = 0;
    for (int i = 0; i < _grpN; i++) verified += _mods[_grp[i]].st == M_VERIFIED;
    note(2, (uint16_t)(((uint16_t)_grpCh << 8) | verified));
    writeRecord(RES_RUNNING);
    _ph = PH_PICK;
}

static void keepalive() {
    if (_grpN && now_() - _lastTx >= KEEPALIVE_MS && parked()) {
        sendChunk(0);
    }
}

// May this module be rebooted now? The bus has been quiet, nobody wrote to it recently, and the rhythm of whoever polls it says
// the next request is not due while it would be resetting (so a polled module is rebooted right after its turn, not before it).
static bool mayApply(uint8_t id) {
    return windowOpen() && rtu_msSinceForeign() >= _applyIdleMs && rtu_msSinceWrite(id) >= _applyQuietMs && rtu_msUntilNextTouch(id) >= APPLY_WINDOW_MS;
}

// ── The state machine: one small step per call ─────────────────────────────
static void step() {
    switch (_ph) {
    case PH_PICK: {
        // between groups: a module that has waited long enough for a quiet moment is applied first
        for (int i = 0; i < _nMods; i++) {
            if (_mods[i].st != M_VERIFIED) continue;
            if (mayApply(_mods[i].id)) {
                _applyIdx = (uint8_t)i;
                _ph = PH_APPLY_SEND;
                return;
            }
        }
        // A window run: when the last night has closed and work is left, the run ends with it recorded; between windows
        // (or too close to the end of one, or to a shelf reset) no new group is started.
        if (windowsExhausted() && (countPending() > 0 || countWaitingApply() > 0)) { finish(RES_FAILED, 6); return; }
        if (countPending() > 0 && !groupMayStart()) { _waitUntil = now_() + 1000; return; }
        if (pickGroup()) { _pos = 0; _sub = 0; _ph = PH_PROBE; return; }
        if (countWaitingApply() > 0) { _ph = PH_WAIT_APPLY; return; }
        const bool ok = countFailed() == 0 && countPending() == 0;
        finish(ok ? RES_OK : RES_FAILED, ok ? 5 : 6);
        return;
    }
    case PH_WAIT_APPLY: {
        // everything is staged; the rest waits for the bus and the modules to be quiet
        if (windowsExhausted() && countWaitingApply() > 0) { finish(RES_FAILED, 6); return; }
        for (int i = 0; i < _nMods; i++) {
            if (_mods[i].st != M_VERIFIED) continue;
            if (mayApply(_mods[i].id)) {
                _applyIdx = (uint8_t)i;
                _ph = PH_APPLY_SEND;
                return;
            }
        }
        if (countWaitingApply() == 0) _ph = PH_PICK;
        _waitUntil = now_() + 1000;
        return;
    }
    case PH_PROBE: {
        // Parked first: on a new channel the first frame is the hub's crossing trigger, which is lost by design.
        if (!mayTransmit(_idleMs) || !parked()) return;
        Mod& m = _mods[_grp[_pos]];
        uint16_t r[5];
        if (readRegs(m.id, 0, 5, r)) { m.fwBefore = m.fwBefore ? m.fwBefore : r[1]; m.err = 0; _sub = 0; }
        else if (++_sub < 3) return;                  // one missed read is not a silent module: asked again next pass
        else { m.st = M_NO_REPLY; m.err = ERR_NO_REPLY; _sub = 0; }
        if (++_pos >= _grpN) {
            // keep only the members that answered
            uint8_t k = 0;
            for (int i = 0; i < _grpN; i++) if (_mods[_grp[i]].st == M_PENDING) _grp[k++] = _grp[i];
            _grpN = k;
            _ph = _grpN ? PH_META : PH_PICK;
            if (!_grpN) groupDone();
        }
        return;
    }
    case PH_META: {
        if (!mayTransmit(_idleMs) || !parked()) return;
        _total = (uint16_t)((_imgSize + CHUNK_SIZE - 1) / CHUNK_SIZE);
        const uint16_t v[5] = {(uint16_t)(_imgSize >> 16), (uint16_t)_imgSize, (uint16_t)(_imgCrc >> 16), (uint16_t)_imgCrc, _total};
        sendBroadcastRegs(REG_META_FIRST, v, 5);
        _pos = 0;
        _ph = PH_ENTER;
        return;
    }
    case PH_ENTER: {
        if (!mayTransmit(_idleMs)) return;
        writeCoil(_mods[_grp[_pos]].id, COIL_ENTER);     // unicast: only this group enters the session
        _waitUntil = now_() + ENTER_ERASE_MS;
        if (++_pos >= _grpN) { _pos = 0; _ph = PH_ENTER_CHECK; }
        return;
    }
    case PH_ENTER_CHECK: {
        if (!mayTransmit(_idleMs)) return;
        Mod& m = _mods[_grp[_pos]];
        uint16_t r[2] = {0, 0};
        if (readRegs(m.id, REG_STATE, 2, r) && (r[0] & 0xFF) == 1) m.st = M_SESSION;
        else { m.st = M_PENDING; m.tries++; m.err = (uint8_t)(r[0] >> 8 ? r[0] >> 8 : ERR_NOT_ENTERED); if (m.tries >= MAX_TRIES) { m.st = M_GAVE_UP; m.err = ERR_GAVE_UP; } }
        if (++_pos >= _grpN) {
            uint8_t k = 0;
            for (int i = 0; i < _grpN; i++) if (_mods[_grp[i]].st == M_SESSION) _grp[k++] = _grp[i];
            _grpN = k;
            if (!_grpN) { groupDone(); return; }
            _idx = 0; _sent = 0;
            _ph = PH_STREAM;
        }
        return;
    }
    case PH_STREAM: {
        if (!mayTransmit(_idleMs) || !parked()) return;
        sendChunk(_idx);
        if (++_idx >= _total) { _round = 1; _pos = 0; _sub = 0; _repairN = 0; memset(_missing, 0, sizeof(_missing));
                                _ph = PH_REPAIR; return; }
        if (_sent % resyncEvery() == 0) _waitUntil = now_() + resyncPause();
        return;
    }
    case PH_REPAIR: {
        // per member: state, then bitmap. A member that is complete is done; one that left the session is dropped.
        if (!mayTransmit(_idleMs)) return;
        if (_pos >= _grpN) {
            // Every member has been looked at (read, skipped as complete, or dropped): the chunks still missing anywhere.
            _repairN = 0;
            for (uint16_t i = 0; i < _total; i++)
                if ((_missing[i / 8] >> (i % 8)) & 1) _repairList[_repairN++] = i;
            if (_repairN == 0) {
                _ph = PH_FINALIZE;
                return;
            }
            if (_round >= REPAIR_ROUNDS) {
                _ph = PH_FINALIZE;      // whoever is still incomplete is dropped at the finalize check
                return;
            }
            _idx = 0;
            _ph = PH_REPAIR_SEND;
            return;
        }
        Mod& m = _mods[_grp[_pos]];
        if (m.st != M_SESSION) { _pos++; return; }
        if (!parked()) return;
        keepalive();
        uint16_t st[2] = {0, 0};
        if (!readRegs(m.id, REG_STATE, 2, st) || (st[0] & 0xFF) != 1) {
            dropMember(_pos, (uint8_t)(st[0] >> 8));
            _pos++;
            return;
        }
        uint16_t bm[BITMAP_REGS];
        if (!readRegs(m.id, REG_BITMAP_FIRST, BITMAP_REGS, bm)) {              // asked again next pass, three times at most
            if (++_sub >= 3) { dropMember(_pos, ERR_NO_REPLY); _sub = 0; _pos++; }
            return;
        }
        _sub = 0;
        bool complete = true;
        for (uint16_t i = 0; i < _total; i++) {
            if (!((bm[i / 16] >> (i % 16)) & 1)) { complete = false; _missing[i / 8] |= (uint8_t)(1u << (i % 8)); }
        }
        if (complete) m.st = M_COMPLETE;
        _pos++;
        return;
    }
    case PH_REPAIR_SEND: {
        if (!mayTransmit(_idleMs) || !parked()) return;
        sendChunk(_repairList[_idx]);
        if (++_idx >= _repairN) {
            _round++; _pos = 0; _repairN = 0; memset(_missing, 0, sizeof(_missing));
            _ph = PH_REPAIR;
            return;
        }
        if (_sent % resyncEvery() == 0) _waitUntil = now_() + resyncPause();
        return;
    }
    case PH_FINALIZE: {
        if (!mayTransmit(_idleMs) || !parked()) return;
        keepalive();
        sendBroadcastCoil(COIL_FINALIZE);      // only modules that are RECEIVING act on it
        _waitUntil = now_() + FINALIZE_MS;
        _pos = 0; _sub = 0;
        _ph = PH_FINALIZE_CHECK;
        return;
    }
    case PH_FINALIZE_CHECK: {
        if (!mayTransmit(_idleMs)) return;
        if (_pos >= _grpN) { groupDone(); return; }
        Mod& m = _mods[_grp[_pos]];
        if (m.st != M_SESSION && m.st != M_COMPLETE) { _pos++; return; }
        uint16_t r[2];
        if (!readRegs(m.id, REG_STATE, 2, r)) {                  // asked again next pass, three times at most
            if (++_sub >= 3) { dropMember(_pos, ERR_NO_REPLY); _sub = 0; _pos++; }
            return;
        }
        _sub = 0;
        const uint8_t state = r[0] & 0xFF, err = (uint8_t)(r[0] >> 8);
        if (state == 2) { m.st = M_VERIFIED; m.err = 0; }
        else if (state == 3 && err == 4) dropMember(_pos, err);   // timed out waiting: a pacing casualty, retried slow
        else if (state == 3) { m.st = M_FAILED; m.err = err; }
        else dropMember(_pos, err ? err : ERR_NOT_ENTERED);       // still receiving but incomplete
        _pos++;
        return;
    }
    case PH_APPLY_SEND: {
        Mod& m = _mods[_applyIdx];
        // re-check at the moment of sending: the bus and the module must still be quiet
        if (!mayApply(m.id)) { _ph = PH_PICK; return; }
        writeCoil(m.id, COIL_APPLY);                // may reset before it answers
        _waitUntil = now_() + APPLY_WAIT_MS;
        _confirmTry = 0;
        _ph = PH_APPLY_CONFIRM;
        return;
    }
    case PH_APPLY_CONFIRM: {
        if (!mayTransmit(_idleMs)) return;
        Mod& m = _mods[_applyIdx];
        uint16_t r[1];
        if (readRegs(m.id, 1, 1, r)) {
            m.fwAfter = r[0];
            m.st = (r[0] != m.fwBefore) ? M_UPDATED : M_SAME;
            m.err = 0;
            note(4, (uint16_t)(((uint16_t)m.id << 8) | (m.st == M_UPDATED)));
            writeRecord(RES_RUNNING);
            _ph = PH_PICK;
            return;
        }
        if (++_confirmTry >= CONFIRM_TRIES) {
            m.st = M_FAILED; m.err = ERR_NO_REPLY_POST;
            note(4, (uint16_t)((uint16_t)m.id << 8));
            writeRecord(RES_RUNNING);
            _ph = PH_PICK;
            return;
        }
        _waitUntil = now_() + 1000;
        return;
    }
    default:
        _ph = PH_PICK;
    }
}

void otaRunner_begin() {}

bool otaRunner_running() { return _run == RUN_ACTIVE; }

static void doAbort() {
    // Tell every module that holds a session or a staged image, one by one (a broadcast abort would blank the display of
    // the others too): the current group, and the modules of earlier groups that are verified and waiting to apply.
    for (int i = 0; i < _nMods; i++) {
        Mod& m = _mods[i];
        if (m.st == M_SESSION || m.st == M_COMPLETE || m.st == M_VERIFIED) {
            writeCoil(m.id, COIL_ABORT);
            m.st = M_PENDING;
            gwStatus_watchdogKick();                // up to 80 unicast writes in one loop pass
        }
    }
    _abortReq = false;
    finish(RES_ABORTED, 7);
}

void otaRunner_update() {
    if (_run != RUN_ACTIVE) return;
    if (_abortReq) { doAbort(); return; }
    if (_winOn) {
        const bool in = inWindow();
        if (in && !_winPrev) note(8, (uint16_t)(_nightsDone + 1));                         // a window opened
        if (!in && _winPrev) { _nightsDone++; note(9, (uint16_t)(countPending() + countWaitingApply())); }   // one closed
        _winPrev = in;
    }
    if (!due(_waitUntil)) return;
    step();
}

// ── Console ────────────────────────────────────────────────────────────────
static char _line[160];
static void out(void (*emit)(const char*), const char* fmt, ...) __attribute__((format(printf, 2, 3)));
static void out(void (*emit)(const char*), const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(_line, sizeof(_line), fmt, ap);
    va_end(ap);
    emit(_line);
}

static bool parseIds(const char* s, uint8_t* ids, uint8_t& n) {
    n = 0;
    while (*s) {
        char* e;
        long a = strtol(s, &e, 10);
        if (e == s || a < 1 || a > 247) return false;
        long b = a;
        if (*e == '-') { s = e + 1; b = strtol(s, &e, 10); if (e == s || b < a || b > 247) return false; }
        for (long v = a; v <= b; v++) { if (n >= MAX_MODS) return false; ids[n++] = (uint8_t)v; }
        if (*e == ',') s = e + 1; else if (*e == 0) s = e; else return false;
    }
    return n > 0;
}

static const char* kv(char** argv, int argc, const char* key) {
    const size_t kl = strlen(key);
    for (int i = 0; i < argc; i++)
        if (!strncmp(argv[i], key, kl) && argv[i][kl] == '=') return argv[i] + kl + 1;
    return nullptr;
}

// A whole decimal number in [lo, hi]; an absent key gives `def` (def < 0 = the key is required).
static bool numArg(const char* v, long lo, long hi, long def, long& out) {
    if (!v) { out = def; return def >= 0; }
    char* e;
    const long n = strtol(v, &e, 10);
    if (e == v || *e != 0 || n < lo || n > hi) return false;
    out = n;
    return true;
}

// "0100" / "2359": exactly four digits, a real time of day.
static bool hhmmArg(const char* v, long& out) {
    if (strlen(v) != 4) return false;
    for (int i = 0; i < 4; i++) if (v[i] < '0' || v[i] > '9') return false;
    const long n = strtol(v, nullptr, 10);
    if (n / 100 > 23 || n % 100 > 59) return false;
    out = n;
    return true;
}

static void cmdStart(char** argv, int argc, bool armed, void (*emit)(const char*)) {
    if (!armed) { emit("#ERR OTA err=locked hint=HELLO"); return; }
    if (_run == RUN_ACTIVE) { emit("#ERR OTA err=busy detail=a_run_is_active"); return; }
    if (!sched_timeSet()) { emit("#ERR OTA err=clock_unset hint=TIME"); return; }
    if (usbBridge_msSinceFrame() < 60000UL) { emit("#ERR OTA err=usb_busy detail=a_pc_used_the_usb_bridge_in_the_last_60s"); return; }
    if (gwRemote_fwBusy()) { emit("#ERR OTA err=busy detail=a_gateway_firmware_upload_is_in_progress"); return; }
    long slotV = 0;
    if (!numArg(kv(argv, argc, "slot"), 0, 3, -1, slotV)) { emit("#ERR OTA err=syntax want=slot=0..3"); return; }
    _slot = (uint8_t)slotV;

    uint32_t size = 0, crc = 0;
    uint8_t nl = 0;
    char name[25];
    const int rc = gwRemote_depotImage(_slot, _image, MAX_IMAGE, size, crc, name, nl);
    if (rc != 0) { out(emit, "#ERR OTA err=image detail=%s", rc == 1 ? "slot_not_complete" : rc == 2 ? "too_big_for_a_module" : "storage"); return; }
    if (gwRemote_crc32(0, _image, size) != crc) { emit("#ERR OTA err=image detail=crc_mismatch_on_read"); return; }
    _imgSize = size; _imgCrc = crc; _imgNameLen = nl; memcpy(_imgName, name, nl); _imgName[nl] = 0;

    uint8_t ids[MAX_MODS], n = 0;
    const char* idList = kv(argv, argc, "ids");
    if (idList && strcmp(idList, "all") != 0) {
        if (!parseIds(idList, ids, n)) { emit("#ERR OTA err=syntax want=ids=11-18,21|all"); return; }
    } else {
        const uint16_t cnt = panel_activeSlotCount();
        for (uint16_t i = 0; i < cnt && n < MAX_MODS; i++) ids[n++] = panel_activeSlotAt(i);
        if (n == 0) { emit("#ERR OTA err=no_ids detail=set_panel.cabinet_or_panel.shape_or_pass_ids"); return; }
    }
    // Same limits as the Test Tool's gw_ota.LIMITS; a value outside them is refused, never wrapped or clamped.
    long idle, applyIdle, applyQuiet, group;
    if (!numArg(kv(argv, argc, "idle"), 1, 600, DEF_IDLE_S, idle)) { emit("#ERR OTA err=range key=idle allowed=1-600"); return; }
    if (!numArg(kv(argv, argc, "apply_idle"), 1, 3600, DEF_APPLY_IDLE_S, applyIdle)) { emit("#ERR OTA err=range key=apply_idle allowed=1-3600"); return; }
    if (!numArg(kv(argv, argc, "apply_quiet"), 0, 86400, DEF_APPLY_QUIET_S, applyQuiet)) { emit("#ERR OTA err=range key=apply_quiet allowed=0-86400"); return; }
    if (!numArg(kv(argv, argc, "group"), 1, MAX_GROUP, DEF_GROUP, group)) { emit("#ERR OTA err=range key=group allowed=1-8"); return; }
    _idleMs = (uint32_t)idle * 1000UL;
    _applyIdleMs = (uint32_t)applyIdle * 1000UL;
    _applyQuietMs = (uint32_t)applyQuiet * 1000UL;
    _groupMax = (uint8_t)group;

    // Optional night window: at= and until= together (HHMM on the gateway's wall clock), nights= how many windows to use.
    const char* atV = kv(argv, argc, "at");
    const char* untilV = kv(argv, argc, "until");
    bool winOn = false;
    uint32_t atS = 0, untilS = 0;
    long nights = 1;
    if (atV || untilV) {
        long a, u;
        if (!atV || !untilV || !hhmmArg(atV, a) || !hhmmArg(untilV, u) || a == u) { emit("#ERR OTA err=syntax want=at=HHMM_until=HHMM"); return; }
        if (!numArg(kv(argv, argc, "nights"), 1, WIN_MAX_NIGHTS, 1, nights)) { emit("#ERR OTA err=range key=nights allowed=1-14"); return; }
        winOn = true;
        atS = (uint32_t)(a / 100) * 3600UL + (uint32_t)(a % 100) * 60UL;
        untilS = (uint32_t)(u / 100) * 3600UL + (uint32_t)(u % 100) * 60UL;
    } else if (kv(argv, argc, "nights")) { emit("#ERR OTA err=syntax detail=nights_needs_at_and_until"); return; }

    if (!gwRemote_fsMount()) { emit("#ERR OTA err=storage"); return; }
    _winOn = winOn; _winAtS = atS; _winUntilS = untilS; _nights = (uint8_t)nights; _nightsDone = 0;
    _winPrev = winOn && inWindow();
    _nMods = n;
    for (int i = 0; i < n; i++) {
        _mods[i] = Mod{ids[i], channelOf(ids[i]), (uint8_t)M_PENDING, 0, 0, 0, 0};
    }
    _seq = nextSeq();
    _startEpoch = sched_now(); _endEpoch = 0;
    _result = RES_RUNNING;
    _abortReq = false;
    _grpN = 0; _pausedSince = 0; _pausedTotalMs = 0; _restarts = 0; _waitUntil = now_();
    _ph = PH_PICK;
    _run = RUN_ACTIVE;
    gwRemote_fsHold(true);
    writeRecord(RES_RUNNING);
    note(1, n);
    if (_winOn)
        out(emit, "#OK OTA START run=%lu image=%s size=%lu modules=%u window=%04lu-%04lu nights=%ld", (unsigned long)_seq, _imgName,
            (unsigned long)_imgSize, n, (unsigned long)((_winAtS / 3600) * 100 + (_winAtS % 3600) / 60),
            (unsigned long)((_winUntilS / 3600) * 100 + (_winUntilS % 3600) / 60), nights);
    else
        out(emit, "#OK OTA START run=%lu image=%s size=%lu modules=%u", (unsigned long)_seq, _imgName, (unsigned long)_imgSize, n);
}

// Between windows: nothing in flight and nothing may start (this is what a window run looks like all day).
static bool holding() { return _winOn && _ph == PH_PICK && _grpN == 0 && countPending() > 0 && !groupMayStart(); }

static void cmdStatus(void (*emit)(const char*)) {
    ResHdr h;
    const bool have = readRecord(0, h, nullptr);
    if (_run == RUN_ACTIVE) {
        out(emit, "#DATA state=%s run=%lu phase=%s ch=%u group=%u chunk=%u/%u", holding() ? "scheduled" : "running", (unsigned long)_seq,
            phaseName(_ph), _grpCh, _grpN, (unsigned)_idx, (unsigned)_total);
        if (_winOn)
            out(emit, "#DATA window=%04lu-%04lu nights=%u/%u gate=%s", (unsigned long)((_winAtS / 3600) * 100 + (_winAtS % 3600) / 60),
                (unsigned long)((_winUntilS / 3600) * 100 + (_winUntilS % 3600) / 60), (unsigned)_nightsDone, (unsigned)_nights,
                !inWindow() ? "closed" : sched_resetNear(WIN_BUDGET_S, WIN_RESET_AFTER_S) ? "reset_hold" : groupMayStart() ? "open" : "ending");
        uint32_t paused = _pausedSince ? (now_() - _pausedSince) / 1000 : 0;
        out(emit, "#DATA paused_s=%lu paused_total_s=%lu quiet_s=%lu restarts=%lu", (unsigned long)paused,
            (unsigned long)((_pausedTotalMs + (_pausedSince ? now_() - _pausedSince : 0)) / 1000),
            (unsigned long)(rtu_msSinceForeign() / 1000), (unsigned long)_restarts);
        out(emit, "#DATA image=%s size=%lu crc=%08lX modules=%u pending=%d verified=%d applied=%d failed=%d", _imgName,
            (unsigned long)_imgSize, (unsigned long)_imgCrc, _nMods, countPending(), countWaitingApply(), countApplied(), countFailed());
    } else if (have) {
        const char* s = (h.state == RES_RUNNING) ? resName(RES_INTERRUPTED) : resName(h.state);
        out(emit, "#DATA state=%s run=%lu image=%.*s size=%lu modules=%u", s, (unsigned long)h.seq, h.nameLen, h.name,
            (unsigned long)h.imgSize, h.count);
    } else {
        emit("#DATA state=none");
    }
    emit("#OK OTA STATUS");
}

static void cmdResult(char** argv, int argc, void (*emit)(const char*)) {
    const int n = argc > 0 ? atoi(argv[0]) : 0;
    ResHdr h;
    static ResMod rows[MAX_MODS];
    if (!readRecord(n, h, rows)) { emit("#ERR OTA err=none detail=no_such_run"); return; }
    const char* s = (h.state == RES_RUNNING && !(_run == RUN_ACTIVE && h.seq == _seq)) ? resName(RES_INTERRUPTED) : resName(h.state);
    out(emit, "#DATA run=%lu state=%s image=%.*s size=%lu crc=%08lX start=%lu end=%lu modules=%u", (unsigned long)h.seq, s,
        h.nameLen, h.name, (unsigned long)h.imgSize, (unsigned long)h.imgCrc, (unsigned long)h.startEpoch, (unsigned long)h.endEpoch, h.count);
    for (int i = 0; i < h.count; i++) {
        out(emit, "#DATA id=%u st=%s err=%u tries=%u fw=%u>%u", rows[i].id, modStateName(rows[i].st), rows[i].err, rows[i].tries,
            rows[i].fwBefore, rows[i].fwAfter);
        gwStatus_watchdogKick();
    }
    emit("#OK OTA RESULT");
}

void otaRunner_command(char** argv, int argc, bool armed, void (*emit)(const char* line)) {
    if (argc < 1) { emit("#ERR OTA err=syntax want=START|STATUS|RESULT|ABORT"); return; }
    if (!strcasecmp(argv[0], "START")) cmdStart(argv + 1, argc - 1, armed, emit);
    else if (!strcasecmp(argv[0], "STATUS")) cmdStatus(emit);
    else if (!strcasecmp(argv[0], "RESULT")) cmdResult(argv + 1, argc - 1, emit);
    else if (!strcasecmp(argv[0], "ABORT")) {
        if (!armed) { emit("#ERR OTA err=locked hint=HELLO"); return; }
        if (_run != RUN_ACTIVE) { emit("#ERR OTA err=idle"); return; }
        _abortReq = true;
        emit("#OK OTA ABORT");
    } else emit("#ERR OTA err=syntax want=START|STATUS|RESULT|ABORT");
}

void otaRunner_infoLine(char* out_, size_t n) {
    if (_run == RUN_ACTIVE)
        snprintf(out_, n, "ota.state=%s ota.phase=%s ota.pending=%d ota.applied=%d ota.failed=%d", holding() ? "scheduled" : "running",
                 phaseName(_ph), countPending(), countApplied(), countFailed());
    else
        snprintf(out_, n, "ota.state=idle");
}
