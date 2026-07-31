#ifndef GW_CONFIG_H
#define GW_CONFIG_H

#include <Arduino.h>
#include "config.h"

// ── Gateway settings ───────────────────────────────────────────────────────
// Two copies live side by side: ACTIVE (what the firmware is running on) and
// STAGED (what the console has been editing). `SET` only ever touches staged;
// `SAVE` validates it, persists it and applies whatever can change live.
//
// sys.log is deliberately NOT part of this struct: it is volatile and resets
// every boot, so a unit can never ship with logging on and corrupt the host's
// binary Modbus stream.

struct GwConfig {
    char     sys_name[16];
    uint32_t rs485_baud;
    uint32_t rs485_pre_us;
    uint32_t rs485_post_us;
    uint16_t rs485_t1_ms;
    uint16_t rs485_t2_ms;
    uint16_t usb_gap_ms;
    uint16_t usb_max_ms;
    uint8_t  net_enabled;
    uint8_t  net_dhcp;
    uint32_t net_ip;
    uint32_t net_mask;
    uint32_t net_gw;
    uint32_t net_dns;
    uint16_t net_port;
    uint16_t net_link_timeout_ms;
};

enum class GwSource : uint8_t { STORED, DEFAULTS, CORRUPT, UNAVAILABLE, MIGRATED };

// Load from the store (or defaults) and apply. forceDefaults skips the store
// entirely — used by safe mode and the button recovery hold.
void gwConfig_begin(bool forceDefaults);

const GwConfig& gwConfig_active();
const GwConfig& gwConfig_staged();
GwSource        gwConfig_source();
const char*     gwConfig_sourceName();

// Key table (drives GET / HELP / SET)
int         gwConfig_keyCount();
const char* gwConfig_keyName(int index);
int         gwConfig_indexOf(const char* key);      // -1 unknown; accepts an
                                                    // unambiguous suffix ("ip")
bool        gwConfig_isReadOnly(int index);
bool        gwConfig_needsReboot(int index);
bool        gwConfig_differs(int index);            // staged != active
int         gwConfig_dirtyCount();

// Render a value as text. staged=false renders the active copy.
bool gwConfig_format(int index, bool staged, char* out, size_t n);

// Parse and range-check one value into staged.
// Returns 0 on success, or fills `err` with a machine-readable reason.
int gwConfig_set(int index, const char* value, char* err, size_t errN);

// Cross-field checks over the whole staged struct (mask contiguity, gateway in
// subnet, t2 < t1, gap < max). 0 = ok.
int gwConfig_validateStaged(char* detail, size_t n);

// Validate → persist → apply live → active := staged.
// `applied` lists the groups that took effect now, `pending` the keys that
// need a reboot. Returns false if validation or the store failed.
bool gwConfig_commit(char* applied, size_t appliedN,
                     char* pending, size_t pendingN, char* err, size_t errN);

void gwConfig_discard();                // staged := active
void gwConfig_loadDefaultsToStaged();   // staged := factory (not committed)
void gwConfig_applyLive();              // push active into RS485 / USB framing

#endif // GW_CONFIG_H
