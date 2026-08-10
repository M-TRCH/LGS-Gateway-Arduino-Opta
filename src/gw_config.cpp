#include "gw_config.h"
#include "panel.h"

#include "gw_status.h"
#include "gw_store.h"
#include "modbus_rtu.h"
#include "net_runtime.h"
#include "usb_bridge.h"

// ── Key table ──────────────────────────────────────────────────────────────
enum GwKind : uint8_t { KIND_STR, KIND_BOOL, KIND_U16, KIND_U32, KIND_IP, KIND_MAC_RO,
                        KIND_HUBMAP };

struct KeyDef {
    const char* name;
    GwKind      kind;
    uint32_t    lo, hi;      // inclusive range for numeric kinds
    bool        reboot;      // takes effect only after a restart
};

static const KeyDef KEYS[] = {
    { "sys.name",             KIND_STR,  0, 0,          false },
    { "rs485.baud",           KIND_U32,  9600, 57600,   false },
    { "rs485.predelay_us",    KIND_U32,  0, 50000,      false },
    { "rs485.postdelay_us",   KIND_U32,  0, 50000,      false },
    { "rs485.t1_ms",          KIND_U16,  20, 2000,      false },
    { "rs485.t2_ms",          KIND_U16,  5, 200,        false },
    { "usb.gap_ms",           KIND_U16,  3, 50,         false },
    { "usb.max_ms",           KIND_U16,  20, 500,       false },
    { "net.enabled",          KIND_BOOL, 0, 1,          true  },
    { "net.dhcp",             KIND_BOOL, 0, 1,          true  },
    { "net.ip",               KIND_IP,   0, 0,          true  },
    { "net.mask",             KIND_IP,   0, 0,          true  },
    { "net.gw",               KIND_IP,   0, 0,          true  },
    { "net.dns",              KIND_IP,   0, 0,          true  },
    { "net.port",             KIND_U16,  1, 65535,      false },
    { "net.link_timeout_ms",  KIND_U16,  500, 10000,    true  },
    { "net.mac",              KIND_MAC_RO, 0, 0,        false },
    // Appended, never inserted: valueOf()/storeValue() switch on the index.
    { "bus.hub_map",          KIND_HUBMAP, 0, 0,        false },
    { "bus.hub_retry",        KIND_U16,  1, 10,         false },
    { "bus.hub_gap_ms",       KIND_U16,  0, 1000,       false },
    { "bus.hub_budget_ms",    KIND_U16,  0, 4000,       false },
    { "bus.hub_settle_ms",    KIND_U16,  200, 5000,     false },
    { "panel.enabled",        KIND_BOOL, 0, 1,          false },
    { "panel.cabinet",        KIND_U16,  40, 80,        false },
    { "panel.btn1",           KIND_U16,  0, 4,          false },
    { "panel.btn2",           KIND_U16,  0, 4,          false },
    { "panel.btn3",           KIND_U16,  0, 4,          false },
    { "panel.btn4",           KIND_U16,  0, 4,          false },
    { "panel.btn5",           KIND_U16,  0, 4,          false },
    { "panel.step_ms",        KIND_U16,  0, 2000,       false },
    { "panel.reset_ms",       KIND_U16,  200, 10000,    false },
    { "panel.lamps",          KIND_BOOL, 0, 1,          false },
    { "panel.lamp_hold_ms",   KIND_U16,  100, 5000,     false },
    { "panel.lamp_dwell_ms",  KIND_U16,  100, 2000,     false },
    { "panel.lamp_dead",      KIND_U16,  1, 100,        false },
    { "panel.out2",           KIND_U16,  0, 7,          false },
    { "panel.out3",           KIND_U16,  0, 7,          false },
    { "panel.out4",           KIND_U16,  0, 7,          false },
};
static const int KEY_N = (int)(sizeof(KEYS) / sizeof(KEYS[0]));

static const uint32_t BAUD_WHITELIST[] = { 9600, 19200, 38400, 57600 };

static GwConfig _active, _staged;
static GwSource _source = GwSource::DEFAULTS;

// ── Helpers ────────────────────────────────────────────────────────────────
static void defaults(GwConfig& c) {
    memset(&c, 0, sizeof(c));
    c.sys_name[0]          = '\0';
    c.rs485_baud           = DEF_RS485_BAUD;
    c.rs485_pre_us         = DEF_RS485_PRE_DELAY_US;
    c.rs485_post_us        = DEF_RS485_POST_DELAY_US;
    c.rs485_t1_ms          = DEF_TIMEOUT_FIRST_BYTE_MS;
    c.rs485_t2_ms          = DEF_TIMEOUT_INTER_BYTE_MS;
    c.usb_gap_ms           = DEF_USB_FRAME_GAP_MS;
    c.usb_max_ms           = DEF_USB_FRAME_MAX_MS;
    c.net_enabled          = DEF_NET_ENABLED;
    c.net_dhcp             = 0;
    c.net_ip               = DEF_NET_IP;
    c.net_mask             = DEF_NET_MASK;
    c.net_gw               = DEF_NET_GW;
    c.net_dns              = DEF_NET_DNS;
    c.net_port             = DEF_NET_PORT;
    c.net_link_timeout_ms  = DEF_NET_LINK_TIMEOUT_MS;
    // No hub by default: an all-zero map makes every row channel 0, so
    // nothing ever counts as a channel change and a gateway wired
    // straight to the bus behaves exactly as it did before this existed.
    memset(c.hub_map, 0, sizeof(c.hub_map));
    c.hub_retry            = DEF_HUB_RETRY;
    c.hub_settle_ms        = DEF_HUB_SETTLE_MS;
    c.hub_gap_ms           = DEF_HUB_GAP_MS;
    c.hub_budget_ms        = DEF_HUB_BUDGET_MS;
    // Panel buttons: red lights the cabinet, green clears it, blue adds the
    // latch, white power-cycles it. Yellow is left unassigned until a site
    // says what it is for.
    c.panel_enabled        = DEF_PANEL_ENABLED;
    c.panel_cabinet        = DEF_PANEL_CABINET;
    c.panel_btn[0]         = 1;    // red    -> all_on
    c.panel_btn[1]         = 2;    // green  -> all_off
    c.panel_btn[2]         = 3;    // blue   -> all_unlock
    c.panel_btn[3]         = 0;    // yellow -> unassigned
    c.panel_btn[4]         = 4;    // white  -> reset
    c.panel_step_ms        = DEF_PANEL_STEP_MS;
    c.panel_reset_ms       = DEF_PANEL_RESET_MS;
    c.panel_lamps          = DEF_PANEL_LAMPS;
    c.panel_lamp_hold_ms   = DEF_PANEL_LAMP_HOLD_MS;
    c.panel_lamp_dwell_ms  = DEF_PANEL_LAMP_DWELL_MS;
    c.panel_lamp_dead      = DEF_PANEL_LAMP_DEAD;
    c.panel_out[0]         = DEF_PANEL_OUT2;
    c.panel_out[1]         = DEF_PANEL_OUT3;
    c.panel_out[2]         = DEF_PANEL_OUT4;
}

static bool parseIp(const char* s, uint32_t* out) {
    unsigned a, b, c, d;
    char tail;
    if (sscanf(s, "%u.%u.%u.%u%c", &a, &b, &c, &d, &tail) != 4) return false;
    if (a > 255 || b > 255 || c > 255 || d > 255) return false;
    *out = ((uint32_t)a << 24) | ((uint32_t)b << 16) | ((uint32_t)c << 8) | d;
    return true;
}

static void formatIp(uint32_t ip, char* out, size_t n) {
    snprintf(out, n, "%u.%u.%u.%u", (unsigned)(ip >> 24) & 0xFF,
             (unsigned)(ip >> 16) & 0xFF, (unsigned)(ip >> 8) & 0xFF,
             (unsigned)ip & 0xFF);
}

static bool parseU32(const char* s, uint32_t* out) {
    if (!*s) return false;
    char* end = nullptr;
    unsigned long v = strtoul(s, &end, 10);
    if (end == s || (end && *end != '\0')) return false;
    *out = (uint32_t)v;
    return true;
}

static uint32_t valueOf(const GwConfig& c, int i) {
    switch (i) {
        case 1:  return c.rs485_baud;
        case 2:  return c.rs485_pre_us;
        case 3:  return c.rs485_post_us;
        case 4:  return c.rs485_t1_ms;
        case 5:  return c.rs485_t2_ms;
        case 6:  return c.usb_gap_ms;
        case 7:  return c.usb_max_ms;
        case 8:  return c.net_enabled;
        case 9:  return c.net_dhcp;
        case 10: return c.net_ip;
        case 11: return c.net_mask;
        case 12: return c.net_gw;
        case 13: return c.net_dns;
        case 14: return c.net_port;
        case 15: return c.net_link_timeout_ms;
        case 18: return c.hub_retry;
        case 19: return c.hub_gap_ms;
        case 20: return c.hub_budget_ms;
        case 21: return c.hub_settle_ms;
        case 22: return c.panel_enabled;
        case 23: return c.panel_cabinet;
        case 24: return c.panel_btn[0];
        case 25: return c.panel_btn[1];
        case 26: return c.panel_btn[2];
        case 27: return c.panel_btn[3];
        case 28: return c.panel_btn[4];
        case 29: return c.panel_step_ms;
        case 30: return c.panel_reset_ms;
        case 31: return c.panel_lamps;
        case 32: return c.panel_lamp_hold_ms;
        case 33: return c.panel_lamp_dwell_ms;
        case 34: return c.panel_lamp_dead;
        case 35: return c.panel_out[0];
        case 36: return c.panel_out[1];
        case 37: return c.panel_out[2];
        default: return 0;
    }
}

static void storeValue(GwConfig& c, int i, uint32_t v) {
    switch (i) {
        case 1:  c.rs485_baud          = v; break;
        case 2:  c.rs485_pre_us        = v; break;
        case 3:  c.rs485_post_us       = v; break;
        case 4:  c.rs485_t1_ms         = (uint16_t)v; break;
        case 5:  c.rs485_t2_ms         = (uint16_t)v; break;
        case 6:  c.usb_gap_ms          = (uint16_t)v; break;
        case 7:  c.usb_max_ms          = (uint16_t)v; break;
        case 8:  c.net_enabled         = (uint8_t)v; break;
        case 9:  c.net_dhcp            = (uint8_t)v; break;
        case 10: c.net_ip              = v; break;
        case 11: c.net_mask            = v; break;
        case 12: c.net_gw              = v; break;
        case 13: c.net_dns             = v; break;
        case 14: c.net_port            = (uint16_t)v; break;
        case 15: c.net_link_timeout_ms = (uint16_t)v; break;
        case 18: c.hub_retry           = (uint8_t)v; break;
        case 19: c.hub_gap_ms          = (uint16_t)v; break;
        case 20: c.hub_budget_ms       = (uint16_t)v; break;
        case 21: c.hub_settle_ms       = (uint16_t)v; break;
        case 22: c.panel_enabled       = (uint8_t)v; break;
        case 23: c.panel_cabinet       = (uint16_t)v; break;
        case 24: c.panel_btn[0]        = (uint8_t)v; break;
        case 25: c.panel_btn[1]        = (uint8_t)v; break;
        case 26: c.panel_btn[2]        = (uint8_t)v; break;
        case 27: c.panel_btn[3]        = (uint8_t)v; break;
        case 28: c.panel_btn[4]        = (uint8_t)v; break;
        case 29: c.panel_step_ms       = (uint16_t)v; break;
        case 30: c.panel_reset_ms      = (uint16_t)v; break;
        case 31: c.panel_lamps         = (uint8_t)v; break;
        case 32: c.panel_lamp_hold_ms  = (uint16_t)v; break;
        case 33: c.panel_lamp_dwell_ms = (uint16_t)v; break;
        case 34: c.panel_lamp_dead     = (uint16_t)v; break;
        case 35: c.panel_out[0]        = (uint8_t)v; break;
        case 36: c.panel_out[1]        = (uint8_t)v; break;
        case 37: c.panel_out[2]        = (uint8_t)v; break;
        default: break;
    }
}

// ── Lifecycle ──────────────────────────────────────────────────────────────
void gwConfig_begin(bool forceDefaults) {
    defaults(_active);

    if (forceDefaults) {
        _source = GwSource::DEFAULTS;
    } else {
        GwConfig loaded;
        switch (gwStore_load(loaded)) {
            case GwStoreStatus::OK:
                _active = loaded;
                _source = GwSource::STORED;
                break;
            case GwStoreStatus::NO_RECORD:
                _source = GwSource::DEFAULTS;
                break;
            case GwStoreStatus::CORRUPT:
                _source = GwSource::CORRUPT;
                gwStatus_setFault(true);
                break;
            case GwStoreStatus::UNAVAILABLE:
            default:
                _source = GwSource::UNAVAILABLE;
                gwStatus_setFault(true);
                break;
        }
    }

    _staged = _active;
    gwConfig_applyLive();
}

const GwConfig& gwConfig_active() { return _active; }
const GwConfig& gwConfig_staged() { return _staged; }
GwSource        gwConfig_source() { return _source; }

const char* gwConfig_sourceName() {
    switch (_source) {
        case GwSource::STORED:      return "stored";
        case GwSource::DEFAULTS:    return "defaults";
        case GwSource::CORRUPT:     return "corrupt";
        case GwSource::MIGRATED:    return "migrated";
        case GwSource::UNAVAILABLE: default: return "unavailable";
    }
}

// ── Key table access ───────────────────────────────────────────────────────
int         gwConfig_keyCount()          { return KEY_N; }
const char* gwConfig_keyName(int i)      { return (i >= 0 && i < KEY_N) ? KEYS[i].name : ""; }
bool        gwConfig_isReadOnly(int i)   { return (i >= 0 && i < KEY_N) && KEYS[i].kind == KIND_MAC_RO; }
bool        gwConfig_needsReboot(int i)  { return (i >= 0 && i < KEY_N) && KEYS[i].reboot; }

int gwConfig_indexOf(const char* key) {
    for (int i = 0; i < KEY_N; i++) {
        if (strcasecmp(key, KEYS[i].name) == 0) return i;
    }
    // Accept an unambiguous suffix: "ip" resolves to "net.ip".
    int hit = -1;
    for (int i = 0; i < KEY_N; i++) {
        const char* dot = strchr(KEYS[i].name, '.');
        if (dot && strcasecmp(key, dot + 1) == 0) {
            if (hit >= 0) return -1;            // ambiguous
            hit = i;
        }
    }
    return hit;
}

bool gwConfig_differs(int i) {
    if (i < 0 || i >= KEY_N) return false;
    if (KEYS[i].kind == KIND_MAC_RO) return false;
    if (i == 0) return strncmp(_active.sys_name, _staged.sys_name, sizeof(_active.sys_name)) != 0;
    if (KEYS[i].kind == KIND_HUBMAP)
        return memcmp(_active.hub_map, _staged.hub_map, sizeof(_active.hub_map)) != 0;
    return valueOf(_active, i) != valueOf(_staged, i);
}

int gwConfig_dirtyCount() {
    int n = 0;
    for (int i = 0; i < KEY_N; i++) if (gwConfig_differs(i)) n++;
    return n;
}

bool gwConfig_format(int i, bool staged, char* out, size_t n) {
    if (i < 0 || i >= KEY_N || n == 0) return false;
    const GwConfig& c = staged ? _staged : _active;
    switch (KEYS[i].kind) {
        case KIND_STR:
            snprintf(out, n, "%s", c.sys_name);
            return true;
        case KIND_HUBMAP: {
            // Row 1..N as a comma list, so the wiring reads back the way
            // someone would describe it at the cabinet. Trailing zeros are
            // dropped: not every LGS has ten rows, and a five-row cabinet
            // should read "1,1,1,1,1" rather than pad five rows of nothing.
            // Zeros *between* rows are kept — those say the row is not behind
            // the hub, which is wiring, not absence.
            int last = -1;
            for (int r = 0; r < GW_HUB_MAX_ROWS; r++) {
                if (c.hub_map[r]) last = r;
            }
            if (last < 0) {                     // no hub anywhere
                snprintf(out, n, "0");
                return true;
            }
            size_t at = 0;
            out[0] = ' ';
            for (int r = 0; r <= last && at + 4 < n; r++) {
                at += snprintf(out + at, n - at, r ? ",%u" : "%u",
                               (unsigned)c.hub_map[r]);
            }
            return true;
        }
        case KIND_IP:
            formatIp(valueOf(c, i), out, n);
            return true;
        case KIND_MAC_RO: {
            const uint8_t* m = gwStatus_mac();
            snprintf(out, n, "%02X:%02X:%02X:%02X:%02X:%02X",
                     m[0], m[1], m[2], m[3], m[4], m[5]);
            return true;
        }
        default:
            snprintf(out, n, "%lu", (unsigned long)valueOf(c, i));
            return true;
    }
}

// ── Set / validate ─────────────────────────────────────────────────────────
int gwConfig_set(int i, const char* value, char* err, size_t errN) {
    if (i < 0 || i >= KEY_N) {
        snprintf(err, errN, "err=unknown_key");
        return -1;
    }
    const KeyDef& k = KEYS[i];

    if (k.kind == KIND_MAC_RO) {
        snprintf(err, errN, "err=readonly key=%s", k.name);
        return -1;
    }

    if (k.kind == KIND_STR) {
        for (const char* p = value; *p; p++) {
            if (*p < 0x20 || *p > 0x7E) {
                snprintf(err, errN, "err=range key=%s allowed=printable", k.name);
                return -1;
            }
        }
        if (strlen(value) >= sizeof(_staged.sys_name)) {
            snprintf(err, errN, "err=range key=%s allowed=max15chars", k.name);
            return -1;
        }
        snprintf(_staged.sys_name, sizeof(_staged.sys_name), "%s", value);
        return 0;
    }

    if (k.kind == KIND_HUBMAP) {
        // "1,2,3,4,5,6,7,8,1,2" — row 1..N to hub channel, 0 = not behind the
        // hub. Parsed into a scratch copy first, so a bad entry half way along
        // cannot leave the staged map describing wiring that exists nowhere.
        uint8_t scratch[GW_HUB_MAX_ROWS];
        memset(scratch, 0, sizeof(scratch));
        int row = 0;
        const char* p = value;
        while (*p && row < GW_HUB_MAX_ROWS) {
            char* end = nullptr;
            long ch = strtol(p, &end, 10);
            if (end == p || ch < 0 || ch > GW_HUB_MAX_CH) {
                snprintf(err, errN, "err=range key=%s allowed=0-%d_per_row",
                         k.name, GW_HUB_MAX_CH);
                return -1;
            }
            scratch[row++] = (uint8_t)ch;
            p = end;
            while (*p == ' ') p++;
            if (*p == ',') { p++; while (*p == ' ') p++; }
            else if (*p) {
                snprintf(err, errN, "err=range key=%s allowed=comma_list", k.name);
                return -1;
            }
        }
        if (*p) {
            snprintf(err, errN, "err=range key=%s allowed=max%drows",
                     k.name, GW_HUB_MAX_ROWS);
            return -1;
        }
        memcpy(_staged.hub_map, scratch, sizeof(_staged.hub_map));
        return 0;
    }

    uint32_t v = 0;
    if (k.kind == KIND_IP) {
        if (!parseIp(value, &v)) {
            snprintf(err, errN, "err=range key=%s allowed=a.b.c.d", k.name);
            return -1;
        }
    } else {
        if (!parseU32(value, &v)) {
            snprintf(err, errN, "err=range key=%s allowed=number", k.name);
            return -1;
        }
        if (v < k.lo || v > k.hi) {
            snprintf(err, errN, "err=range key=%s value=%s allowed=%lu-%lu",
                     k.name, value, (unsigned long)k.lo, (unsigned long)k.hi);
            return -1;
        }
        if (i == 1) {                              // rs485.baud whitelist
            bool ok = false;
            for (unsigned j = 0; j < sizeof(BAUD_WHITELIST) / sizeof(BAUD_WHITELIST[0]); j++)
                if (v == BAUD_WHITELIST[j]) ok = true;
            if (!ok) {
                snprintf(err, errN, "err=range key=%s value=%s allowed=9600,19200,38400,57600",
                         k.name, value);
                return -1;
            }
        }
    }

    storeValue(_staged, i, v);
    return 0;
}

static bool contiguousMask(uint32_t m) {
    uint32_t inverted = ~m;
    return (inverted & (inverted + 1)) == 0;       // trailing ones only
}

int gwConfig_validateStaged(char* detail, size_t n) {
    // The cabinet drives which slots a panel sweep walks, and 40/64/80 are
    // the shapes that exist — a number in between would light slots that do
    // not, which reads as a dead row rather than a typo.
    if (_staged.panel_cabinet != 40 && _staged.panel_cabinet != 64 &&
        _staged.panel_cabinet != 80) {
        snprintf(detail, n, "panel.cabinet=40|64|80");
        return -1;
    }
    return 0;
}

// ── Commit / apply ─────────────────────────────────────────────────────────
void gwConfig_applyLive() {
    rtu_setTimeouts(_active.rs485_t1_ms, _active.rs485_t2_ms);
    rtu_setHub(_active.hub_map, _active.hub_retry, _active.hub_gap_ms,
               _active.hub_settle_ms, _active.hub_budget_ms);
    panel_applyConfig();
    usbBridge_setFraming(_active.usb_gap_ms, _active.usb_max_ms);
    RS485.end();
    RS485.setDelays(_active.rs485_pre_us, _active.rs485_post_us);
    RS485.begin(_active.rs485_baud);
    RS485.receive();
    // No-op until the link is up, including the call from gwConfig_begin()
    // that runs before the network exists at all.
    netRuntime_applyPort(_active.net_port);
}

bool gwConfig_commit(char* applied, size_t appliedN,
                     char* pending, size_t pendingN, char* err, size_t errN) {
    char detail[48];
    if (gwConfig_validateStaged(detail, sizeof(detail)) != 0) {
        snprintf(err, errN, "err=validate detail=%s", detail);
        return false;
    }

    // Collect what will change before active is overwritten. Grouping by key
    // prefix keeps this honest as keys are added.
    bool live[5] = { false, false, false, false, false };
    static const char* GROUP[5] = { "rs485", "usb", "net", "bus", "panel" };
    size_t pos = 0;
    if (pendingN) pending[0] = '\0';
    for (int i = 0; i < KEY_N; i++) {
        if (!gwConfig_differs(i)) continue;
        if (gwConfig_needsReboot(i)) {
            pos += snprintf(pending + pos, (pos < pendingN) ? pendingN - pos : 0,
                            "%s%s", pos ? "," : "", gwConfig_keyName(i));
            continue;
        }
        const char* name = gwConfig_keyName(i);
        for (int g = 0; g < 5; g++) {
            size_t n = strlen(GROUP[g]);
            if (strncmp(name, GROUP[g], n) == 0 && name[n] == '.') live[g] = true;
        }
    }

    if (!gwStore_save(_staged)) {
        snprintf(err, errN, "err=store_unavailable");
        return false;
    }

    _active = _staged;
    _source = GwSource::STORED;
    gwConfig_applyLive();

    size_t ap = 0;
    if (appliedN) applied[0] = '\0';
    for (int g = 0; g < 5; g++) {
        if (!live[g]) continue;
        ap += snprintf(applied + ap, (ap < appliedN) ? appliedN - ap : 0,
                       "%s%s", ap ? "," : "", GROUP[g]);
    }
    if (ap == 0) snprintf(applied, appliedN, "none");
    return true;
}

void gwConfig_discard() { _staged = _active; }

void gwConfig_loadDefaultsToStaged() { defaults(_staged); }
