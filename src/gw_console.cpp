#include "gw_console.h"

#include <stdarg.h>
#include <time.h>

#include "event_log.h"
#include "gw_config.h"
#include "modbus_rtu.h"
#include "panel.h"
#include "sched.h"
#include "gw_status.h"
#include "gw_store.h"
#include "net_runtime.h"
#include "ntp.h"
#include "tcp_bridge.h"
#include "version.h"

#define GW_MAGIC        "$LGS "
#define GW_MAGIC_LEN    5
#define GW_LINE_MAX     120
#define GW_LINE_IDLE_MS 5000UL
#define GW_SESSION_MS   120000UL
#define GW_REBOOT_MS    300UL

static char _line[GW_LINE_MAX + 1];
static int  _len;
static bool _inLine;
static unsigned long _lineMs;

static unsigned long _armedUntil;
static unsigned long _rebootAt;

// ── Output ─────────────────────────────────────────────────────────────────
// Normally USB. A remote caller (gw_remote's TCP tunnel) installs a sink for
// the synchronous duration of one dispatch; nothing here is asynchronous, so
// a plain static is race-free — usbBridge_update and tcpBridge_update never
// interleave within a loop pass.
static GwEmitFn _emitFn = nullptr;

static void emit(const char* line) {
    if (_emitFn) { _emitFn(line); return; }
    Serial.print(line);
    Serial.print("\r\n");
}

static void emitf(const char* fmt, ...) {
    char buf[GW_LINE_MAX + 1];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    emit(buf);
}

// ── Session ────────────────────────────────────────────────────────────────
bool gwConsole_armed() {
    return _armedUntil != 0 && (long)(millis() - _armedUntil) < 0;
}

static void arm() {
    _armedUntil = millis() + GW_SESSION_MS;
    gwStatus_setSessionArmed(true);
}

static void disarm() {
    _armedUntil = 0;
    gwStatus_setSessionArmed(false);
    gwConfig_discard();          // never leave staged edits behind
}

static void refreshSession() {
    if (gwConsole_armed()) _armedUntil = millis() + GW_SESSION_MS;
}

// ── Verb handlers ──────────────────────────────────────────────────────────
static void doPing() {
    char serial[16] = "";
    gwStatus_serialHex(serial, sizeof(serial));
    emitf("#OK PING fw=%s hw=opta id=%s up=%lu",
          GW_FW_VERSION, serial, (unsigned long)gwStatus_uptimeS());
}

static void doInfo() {
    char serial[16] = "";
    char mac[24]    = "";
    gwStatus_serialHex(serial, sizeof(serial));
    int macIdx = gwConfig_indexOf("net.mac");
    gwConfig_format(macIdx, false, mac, sizeof(mac));

    // __DATE__ is "Aug 11 2026" — spaces, which the console's key=value
    // reader splits on; `build` arrived as just "Aug" for every version
    // until the site report printed it. Sent with underscores instead.
    char build[16];
    snprintf(build, sizeof(build), "%s", GW_FW_BUILD);
    for (char* p = build; *p; p++) {
        if (*p == ' ') *p = '_';
    }
    emitf("#DATA fw=%s build=%s hw=opta id=%s mac=%s macsrc=%s",
          GW_FW_VERSION, build, serial, mac,
          gwStatus_macValid() ? "board" : "placeholder");
    emitf("#DATA sys.name=%s sys.log=%d sys.up=%lu sys.reset=%s",
          gwConfig_active().sys_name, g_logEnabled ? 1 : 0,
          (unsigned long)gwStatus_uptimeS(), gwStatus_resetReason());
    // sys.wdt is the period actually running, which is not always the one that
    // was asked for — the hardware may have refused it and been given the
    // default instead. Reporting the stored value would hide that.
    emitf("#DATA sys.safe=%d sys.boots=%u sys.btn=%d sys.wdt=%u",
          gwStatus_safeMode() ? 1 : 0, gwStatus_bootAttempts(), gwStatus_buttonRaw(),
          gwStatus_watchdogMs());
    // cfg.why carries gwStore's own reason when the store is down. Without it
    // a failed save says only "store_unavailable", which reads the same for
    // an unformatted QSPI, a missing partition and a corrupt TDBStore — three
    // different repairs. The firmware already knows which; it just never said.
    const char* storeWhy = gwStore_lastError();
    emitf("#DATA cfg.source=%s cfg.store=%s cfg.why=%s cfg.dirty=%d cfg.armed=%d",
          gwConfig_sourceName(), gwStore_available() ? "ok" : "unavailable",
          (storeWhy && *storeWhy) ? storeWhy : "-",
          gwConfig_dirtyCount(), gwConsole_armed() ? 1 : 0);
    const IPAddress ip = netRuntime_localIp();
    // net.client counts (0/1/2 — reads as the old 0/1 for one client);
    // net.peer names them, comma-joined, "-" when none.
    char peers[48];
    tcpBridge_clientDesc(peers, sizeof(peers));
    emitf("#DATA net.state=%s net.ip=%u.%u.%u.%u net.port=%u net.client=%d net.peer=%s",
          netRuntime_stateName(), (unsigned)ip[0], (unsigned)ip[1],
          (unsigned)ip[2], (unsigned)ip[3],
          gwConfig_active().net_port, tcpBridge_clientCount(), peers);
    emitf("#DATA cnt.usb_ok=%lu cnt.usb_drop=%lu cnt.tcp_ok=%lu",
          (unsigned long)gwStatus_get(GW_USB_OK),
          (unsigned long)gwStatus_get(GW_USB_DROP),
          (unsigned long)gwStatus_get(GW_TCP_OK));
    emitf("#DATA cnt.rs485_ok=%lu cnt.rs485_timeout=%lu cnt.cfg_cmd=%lu",
          (unsigned long)gwStatus_get(GW_RS485_OK),
          (unsigned long)gwStatus_get(GW_RS485_TIMEOUT),
          (unsigned long)gwStatus_get(GW_CFG_CMD));
    emitf("#DATA hub.cross=%lu hub.extra=%lu hub.wait_ms=%lu hub.skip=%lu",
          (unsigned long)rtu_hubCross(), (unsigned long)rtu_hubExtra(),
          (unsigned long)rtu_hubWaitMs(), (unsigned long)rtu_hubSkip());
    emitf("#DATA panel.state=%s panel.step=%u/%u panel.in=0x%02X panel.lamp=%s",
          panel_stateName(), panel_progress(), panel_total(),
          (unsigned)panel_inputMask(), panel_lampName());
    char nowStr[24], nextStr[72];       // four times plus a day list
    sched_formatNow(nowStr, sizeof(nowStr));
    sched_describeNext(nextStr, sizeof(nextStr));
    // sched.last answers "did it actually run?" — a reset is over in a
    // second and a half, which nobody is watching at 03:00.
    emitf("#DATA time.now=%s time.set=%d sched.reset=%s sched.last=%lu",
          nowStr, sched_timeSet() ? 1 : 0, nextStr,
          (unsigned long)sched_lastFireEpoch());
    emitf("#DATA ntp.state=%s ntp.last=%lu cnt.ntp_ok=%lu cnt.ntp_fail=%lu",
          ntp_stateName(), (unsigned long)ntp_lastSyncEpoch(),
          (unsigned long)gwStatus_get(GW_NTP_OK),
          (unsigned long)gwStatus_get(GW_NTP_FAIL));
    emitf("#DATA log.state=%s log.n=%lu",
          eventLog_ok() ? "ok" : "off_error",
          (unsigned long)eventLog_count());
    emitf("#DATA rtt.last_ms=%u rtt.max_ms=%u rtt.consec_timeout=%u",
          gwStatus_lastRttMs(), gwStatus_maxRttMs(), gwStatus_consecutiveTimeouts());
    emitf("#OK INFO n=13");
}

// Drive one lamp so the panel's wiring can be checked at the cabinet. Not
// gated on HELLO: it changes nothing that outlives the timeout, and needing a
// session to test a lamp is friction with no safety in it.
static void doLamp(char** argv, int argc) {
    if (argc < 1) { emit("#ERR LAMP err=syntax want=1|2|3|4|off"); return; }
    uint8_t lamp;
    if (!strcasecmp(argv[0], "off")) lamp = PANEL_LAMP_OFF;
    else {
        lamp = (uint8_t)strtoul(argv[0], nullptr, 10);
        if (lamp < 1 || lamp > 4) {
            emit("#ERR LAMP err=range want=1|2|3|4|off");
            return;
        }
    }
    uint32_t ms = (argc >= 2) ? (uint32_t)strtoul(argv[1], nullptr, 10) : 5000UL;
    if (ms < 200UL)   ms = 200UL;
    if (ms > 60000UL) ms = 60000UL;
    panel_forceLamp(lamp, ms);
    emitf("#OK LAMP %s ms=%lu", argv[0], (unsigned long)ms);
}

// Read or set the wall clock. Setting is not gated on HELLO: the Opta loses
// the time whenever it loses power, so the tool sets it the moment it
// connects, and a clock that needs a session to correct is a clock that
// spends its life wrong.
static void doTime(char** argv, int argc) {
    char nowStr[24];
    if (argc >= 1) {
        const uint32_t epoch = (uint32_t)strtoul(argv[0], nullptr, 10);
        if (epoch < 1600000000UL) {         // before 2020: not a wall clock
            emit("#ERR TIME err=range want=epoch_seconds_local");
            return;
        }
        sched_setTime(epoch);
    }
    sched_formatNow(nowStr, sizeof(nowStr));
    emitf("#OK TIME now=%s set=%d epoch=%lu", nowStr, sched_timeSet() ? 1 : 0,
          (unsigned long)sched_now());
}

// Dump the newest N event-log records. Read-only, so ungated like INFO/TIME.
// The USB CDC blocks while a connected host is not reading (a stalled
// terminal mid-dump would otherwise ride out the watchdog), so the dog is
// fed per line.
static void doLog(char** argv, int argc) {
    if (!eventLog_ok()) {
        emit("#ERR LOG err=log_unavailable");
        return;
    }
    uint32_t want = (argc >= 1) ? (uint32_t)strtoul(argv[0], nullptr, 10) : 20UL;
    if (want < 1)   want = 1;
    if (want > 100) want = 100;

    uint32_t shown = 0;
    EventRecord r;
    for (uint32_t back = 0; back < want; back++) {
        if (!eventLog_read(back, r)) break;
        gwStatus_watchdogKick();
        char when[24];
        if (r.epoch) {
            const time_t t = (time_t)r.epoch;
            struct tm tmv;
            gmtime_r(&t, &tmv);         // the clock holds wall time already
            snprintf(when, sizeof(when), "%04d-%02d-%02dT%02d:%02d:%02d",
                     tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday,
                     tmv.tm_hour, tmv.tm_min, tmv.tm_sec);
        } else {
            snprintf(when, sizeof(when), "-");
        }
        emitf("#DATA i=%lu t=%s up=%lu ev=%s a=%u p=%u",
              (unsigned long)r.seq, when, (unsigned long)r.uptimeS,
              eventLog_typeName(r.type), (unsigned)r.aux, (unsigned)r.param);
        shown++;
    }
    emitf("#OK LOG n=%lu total=%lu", (unsigned long)shown,
          (unsigned long)eventLog_count());
}

static void doHelp() {
    emit("#DATA verbs=PING,INFO,HELP,GET,HELLO,BYE,SET,SAVE,DISCARD,DEFAULTS,REBOOT,LAMP,TIME,LOG");
    emit("#DATA note=SET/SAVE/DISCARD/DEFAULTS/REBOOT need HELLO first");
    for (int i = 0; i < gwConfig_keyCount(); i++) {
        emitf("#DATA key=%s%s%s", gwConfig_keyName(i),
              gwConfig_isReadOnly(i) ? " ro=1" : "",
              gwConfig_needsReboot(i) ? " reboot=1" : "");
    }
    emitf("#OK HELP n=%d", gwConfig_keyCount());
}

static void doGetOne(int idx) {
    char active[40], staged[40];
    gwConfig_format(idx, false, active, sizeof(active));
    if (gwConfig_differs(idx)) {
        gwConfig_format(idx, true, staged, sizeof(staged));
        emitf("#OK GET %s=%s staged=%s", gwConfig_keyName(idx), active, staged);
    } else {
        emitf("#OK GET %s=%s", gwConfig_keyName(idx), active);
    }
}

static void doGetAll() {
    char active[40], staged[40];
    for (int i = 0; i < gwConfig_keyCount(); i++) {
        gwConfig_format(i, false, active, sizeof(active));
        if (gwConfig_differs(i)) {
            gwConfig_format(i, true, staged, sizeof(staged));
            emitf("#DATA %s=%s staged=%s", gwConfig_keyName(i), active, staged);
        } else {
            emitf("#DATA %s=%s", gwConfig_keyName(i), active);
        }
    }
    emitf("#OK GET n=%d dirty=%d", gwConfig_keyCount(), gwConfig_dirtyCount());
}

// `sys.log` is handled here, not in GwConfig: it is volatile by design.
static bool setVolatile(const char* key, const char* value) {
    if (strcasecmp(key, "sys.log") != 0 && strcasecmp(key, "log") != 0) return false;
    g_logEnabled = (value[0] == '1');
    return true;
}

static void doSet(char** argv, int argc) {
    int applied = 0;
    char err[80];
    for (int i = 0; i < argc; i++) {
        char* eq = strchr(argv[i], '=');
        if (!eq) {
            emitf("#ERR SET err=syntax arg=%s", argv[i]);
            return;
        }
        *eq = '\0';
        const char* key   = argv[i];
        const char* value = eq + 1;

        if (setVolatile(key, value)) { applied++; continue; }

        int idx = gwConfig_indexOf(key);
        if (idx < 0) {
            emitf("#ERR SET err=unknown_key key=%s", key);
            return;
        }
        if (gwConfig_set(idx, value, err, sizeof(err)) != 0) {
            emitf("#ERR SET %s", err);
            return;
        }
        applied++;
    }
    emitf("#OK SET n=%d dirty=%d", applied, gwConfig_dirtyCount());
}

static void doSave() {
    char applied[32] = "", pending[96] = "", err[80] = "";
    if (!gwConfig_commit(applied, sizeof(applied), pending, sizeof(pending),
                         err, sizeof(err))) {
        emitf("#ERR SAVE %s", err);
        return;
    }
    if (pending[0]) emitf("#OK SAVE applied=%s pending_reboot=%s", applied, pending);
    else            emitf("#OK SAVE applied=%s", applied);
}

// ── Dispatch ───────────────────────────────────────────────────────────────
static void dispatch(char* body) {
    char* argv[12];
    int   argc = 0;
    for (char* tok = strtok(body, " \t"); tok && argc < 12; tok = strtok(nullptr, " \t")) {
        argv[argc++] = tok;
    }
    if (argc == 0) return;

    const char* verb = argv[0];
    char** rest = argv + 1;
    int    restN = argc - 1;

    gwStatus_count(GW_CFG_CMD);
    refreshSession();

    if      (strcasecmp(verb, "PING") == 0) doPing();
    else if (strcasecmp(verb, "INFO") == 0) doInfo();
    else if (strcasecmp(verb, "HELP") == 0) doHelp();
    else if (strcasecmp(verb, "LAMP") == 0) doLamp(rest, restN);
    else if (strcasecmp(verb, "TIME") == 0) doTime(rest, restN);
    else if (strcasecmp(verb, "LOG")  == 0) doLog(rest, restN);
    else if (strcasecmp(verb, "GET")  == 0) {
        if (restN == 0) { doGetAll(); return; }
        int idx = gwConfig_indexOf(rest[0]);
        if (idx < 0) emitf("#ERR GET err=unknown_key key=%s", rest[0]);
        else         doGetOne(idx);
    }
    else if (strcasecmp(verb, "HELLO") == 0) {
        arm();
        emitf("#OK HELLO armed=%lu who=%s", GW_SESSION_MS / 1000UL,
              restN ? rest[0] : "-");
    }
    else if (strcasecmp(verb, "BYE") == 0) {
        disarm();
        emit("#OK BYE");
    }
    else if (strcasecmp(verb, "SET")      == 0 || strcasecmp(verb, "SAVE")   == 0 ||
             strcasecmp(verb, "DISCARD")  == 0 || strcasecmp(verb, "REBOOT") == 0 ||
             strcasecmp(verb, "DEFAULTS") == 0) {
        // Write verbs need an armed session: even a one-in-a-billion false
        // positive from binary traffic then cannot mutate anything.
        if (!gwConsole_armed()) {
            emitf("#ERR %s err=locked hint=HELLO", verb);
            return;
        }
        if      (strcasecmp(verb, "SET")  == 0) doSet(rest, restN);
        else if (strcasecmp(verb, "SAVE") == 0) doSave();
        else if (strcasecmp(verb, "DISCARD") == 0) {
            gwConfig_discard();
            emit("#OK DISCARD dirty=0");
        }
        else if (strcasecmp(verb, "DEFAULTS") == 0) {
            gwConfig_loadDefaultsToStaged();
            emitf("#OK DEFAULTS staged=%d hint=SAVE", gwConfig_dirtyCount());
        }
        else {
            _rebootAt = millis() + GW_REBOOT_MS;
            emitf("#OK REBOOT in=%lu", GW_REBOOT_MS);
        }
    }
    else {
        emitf("#ERR %s err=unknown_verb", verb);
    }
}

static void parseLine() {
    _line[_len] = '\0';
    if (_len > GW_MAGIC_LEN && strncasecmp(_line, GW_MAGIC, GW_MAGIC_LEN) == 0) {
        dispatch(_line + GW_MAGIC_LEN);
    }
    // A line that does not carry the magic is dropped in silence: it was
    // rejected binary traffic, not a command.
    _len = 0;
    _inLine = false;
}

// ── Remote entry ───────────────────────────────────────────────────────────
void gwConsole_execute(char* body, GwEmitFn sink) {
    _emitFn = sink;
    dispatch(body);              // synchronous; cannot throw on this target
    _emitFn = nullptr;
}

void gwConsole_refreshSession() {
    refreshSession();
}

// ── Feed ───────────────────────────────────────────────────────────────────
void gwConsole_begin() {
    _len = 0;
    _inLine = false;
    _armedUntil = 0;
    _rebootAt = 0;
}

void gwConsole_feed(const uint8_t* buf, int len) {
    if (len <= 0) return;

    // Start a line only when a chunk BEGINS with '$'. That keeps a stray '$'
    // inside binary garbage from opening a line.
    if (!_inLine) {
        if (buf[0] != '$') return;
        _inLine = true;
        _len = 0;
        _lineMs = millis();
    }

    for (int i = 0; i < len; i++) {
        uint8_t b = buf[i];
        if (b == '\r' || b == '\n') {
            if (_len > 0) parseLine();
            else { _len = 0; _inLine = false; }
            return;
        }
        if (b < 0x20 || b > 0x7E) {          // not text: abandon the line
            _len = 0;
            _inLine = false;
            return;
        }
        if (_len >= GW_LINE_MAX) {           // overlong: abandon
            _len = 0;
            _inLine = false;
            return;
        }
        _line[_len++] = (char)b;
    }
    _lineMs = millis();
}

void gwConsole_update() {
    if (_inLine && (millis() - _lineMs) > GW_LINE_IDLE_MS) {
        _len = 0;
        _inLine = false;
    }
    if (_armedUntil != 0 && !gwConsole_armed()) disarm();
    if (_rebootAt != 0 && (long)(millis() - _rebootAt) >= 0) {
        Serial.flush();
        NVIC_SystemReset();
    }
}
