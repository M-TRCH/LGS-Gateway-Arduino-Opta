#include "gw_config.h"

#include "gw_status.h"
#include "gw_store.h"
#include "modbus_rtu.h"
#include "net_runtime.h"
#include "usb_bridge.h"

// ── Key table ──────────────────────────────────────────────────────────────
enum GwKind : uint8_t { KIND_STR, KIND_BOOL, KIND_U16, KIND_U32, KIND_IP, KIND_MAC_RO };

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
    const GwConfig& c = _staged;

    if (c.rs485_t2_ms >= c.rs485_t1_ms) {
        snprintf(detail, n, "t2_not_below_t1");
        return -1;
    }
    if (c.usb_gap_ms >= c.usb_max_ms) {
        snprintf(detail, n, "gap_not_below_max");
        return -1;
    }
    if (!c.net_dhcp) {
        if (!contiguousMask(c.net_mask)) {
            snprintf(detail, n, "mask_not_contiguous");
            return -1;
        }
        if (c.net_ip == 0 || (c.net_ip & 0xFF) == 0 || (c.net_ip & 0xFF) == 255) {
            snprintf(detail, n, "ip_not_a_host_address");
            return -1;
        }
        if (c.net_gw != 0 && ((c.net_gw & c.net_mask) != (c.net_ip & c.net_mask))) {
            snprintf(detail, n, "gw_not_in_subnet");
            return -1;
        }
        if (c.net_gw == c.net_ip) {
            snprintf(detail, n, "gw_equals_ip");
            return -1;
        }
    }
    return 0;
}

// ── Commit / apply ─────────────────────────────────────────────────────────
void gwConfig_applyLive() {
    rtu_setTimeouts(_active.rs485_t1_ms, _active.rs485_t2_ms);
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
    bool live[3] = { false, false, false };          // rs485, usb, net
    static const char* GROUP[3] = { "rs485", "usb", "net" };
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
        for (int g = 0; g < 3; g++) {
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
    for (int g = 0; g < 3; g++) {
        if (!live[g]) continue;
        ap += snprintf(applied + ap, (ap < appliedN) ? appliedN - ap : 0,
                       "%s%s", ap ? "," : "", GROUP[g]);
    }
    if (ap == 0) snprintf(applied, appliedN, "none");
    return true;
}

void gwConfig_discard() { _staged = _active; }

void gwConfig_loadDefaultsToStaged() { defaults(_staged); }
