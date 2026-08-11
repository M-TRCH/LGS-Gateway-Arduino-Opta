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

// RS485 switch hub. The bus can run through an 8-channel hub that follows
// traffic: the first frame on a new channel makes it switch, that frame is
// swallowed, and the channel stays deaf for about two seconds (measured on a
// live LGS-64 cabinet — see modbus_rtu.cpp). Two seconds is longer than any
// master's timeout, so the gateway repairs by CLOCK, not by burst: it spends
// the trigger frame, remembers when the channel opens, and holds any earlier
// request in silence until that deadline before sending it.
//
// This has to live in the gateway, not in a client: the hospital's server
// speaks Modbus TCP and knows nothing about a hub, and a lost frame is a
// transport fault, so the layer that owns the wire repairs it.
//
// Which slave sits on which channel is wiring, not arithmetic — a map from
// row (the tens digit of the slave ID) to channel, editable at `bus.hub_map`.
// An all-zero map means "no hub" and disables all of this, which is what a
// gateway wired straight to the bus wants.
#define GW_HUB_MAX_ROWS   10        // slave IDs 11-108 -> rows 1-10
#define GW_HUB_MAX_CH     8

// The sweep shape shares the grid's bounds: rows 1-10, up to 8 slots each.
#define GW_SHAPE_ROWS     10
#define GW_SHAPE_MAX_COLS 8

// How many times a day the shelf may be power-cycled on a schedule. Four
// covers a time per shift with one spare; each slot is armed independently,
// so a site that wants one nightly reset simply leaves the other three off.
#define GW_SCHED_SLOTS    4

struct GwConfig {
    char     sys_name[16];
    // Hardware watchdog period. Reboot-only: the IWDG cannot be reconfigured
    // once it is running.
    uint16_t sys_wdt_ms;
    uint8_t  hub_map[GW_HUB_MAX_ROWS];   // row 1..10 -> channel 1..8, 0 = none
    uint8_t  hub_retry;                  // attempts for the crossing txn itself
    uint16_t hub_gap_ms;                 // margin added past the settle deadline
    uint16_t hub_settle_ms;              // how long the hub stays deaf post-switch
    uint16_t hub_budget_ms;              // ceiling for one transaction, hold included
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
    // Front-panel test buttons (see panel.h). Which colour does what is a
    // site decision, so all five are configurable; 0 = the button is unused.
    uint8_t  panel_enabled;
    uint8_t  panel_btn[5];              // inputs 1-5 -> PanelAction
    uint16_t panel_cabinet;             // 40 / 64 / 80, the sweep's slot list
    uint16_t panel_step_ms;             // pacing between slots in a sweep
    uint16_t panel_reset_ms;            // how long the relays stay dropped
    // Which module preset the sweeps fire (1-8). The preset carries the
    // brightness and colour per module, so the panel's look is tuned there.
    uint8_t  panel_preset;
    // Status lamps. What each output follows is mapped below; these decide
    // when the underlying state changes — how busy a bus has to look before
    // it counts as busy, and how dead before it counts as a fault.
    uint8_t  panel_lamps;               // 0 = leave the lamp outputs alone
    uint16_t panel_lamp_hold_ms;        // traffic this recent keeps amber on
    uint16_t panel_lamp_dwell_ms;       // minimum time between lamp changes
    uint16_t panel_lamp_dead;           // consecutive timeouts before red
    // What each relay output does (PanelSource), outputs 1-4. Which colour
    // sits on which output is wiring, what a colour should mean is a site's
    // call, and even which relay carries the shelf's power is a wiring
    // decision — so all of it lives here rather than in the firmware.
    uint8_t  panel_out[4];              // outputs 1, 2, 3, 4
    // The sweep's own shape: slots per row, rows 1-10, 0 = row absent.
    // All-zero means "follow panel_cabinet's preset", which is what every
    // catalogue cabinet wants — this exists for the cabinet that is not a
    // 40/64/80, so its front-panel buttons can still walk the real slots.
    uint8_t  panel_shape[GW_SHAPE_ROWS];
    // Scheduled power cycle of the shelf. The clock itself is not stored —
    // the Opta cannot keep it through a power cut, so it is set at runtime
    // and the schedule simply does nothing until it has been.
    //
    // Four slots, each armed by its own bit in sched_reset_slots. A time is
    // kept even while its slot is off, so turning a slot back on does not
    // mean typing the hour again — and a disarmed slot can never fire, which
    // a "magic value means unused" scheme could not promise.
    uint8_t  sched_reset_enabled;       // the master switch for all four
    uint16_t sched_reset_hhmm[GW_SCHED_SLOTS];  // 0-2359, wall time
    uint8_t  sched_reset_slots;         // bit0 = slot 1 .. bit3 = slot 4
    uint8_t  sched_reset_days;          // bit0=Sun..bit6=Sat, 0 = every day
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
