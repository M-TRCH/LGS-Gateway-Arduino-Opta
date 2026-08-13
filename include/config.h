#ifndef CONFIG_H
#define CONFIG_H

#include <Arduino.h>

// Values here are compile-time DEFAULTS. Anything a deployment may need to
// change lives in GwConfig (gw_config.h) and is settable at runtime over the
// USB console; these constants only seed the factory defaults.

// ── Hardware Pins ──────────────────────────────────────────────────────────
// The Opta's four relay outputs. O1 cuts the shelf's power for a reset; O2-O4
// are the panel's status lamps, in the order they are wired at the cabinet.
//
// O2 used to be a second power relay for the LED rail (LED_RELAY_PIN). It
// carries a lamp now — one output cannot be both, and a reset that dropped a
// lamp along with the shelf was only ever incidentally right.
#define PANEL_OUT_1         D0          // O1 — the shelf's power by default
// O2-O4 carry the panel lamps. Which colour is on which is wiring, and
// wiring is what gets swapped in a panel, so it is set at panel.lamp_green
// / _amber / _red rather than fixed here.
#define PANEL_OUT_2         D1
#define PANEL_OUT_3         D2
#define PANEL_OUT_4         D3
#define USB_MODE_LED_PIN    LED_USER    // blue front LED: ON = bridge running

// Status LEDs (see gw_status.cpp for what each one means)
#define LED_RS485_PIN       LED_D0      // pulse per RS485 transaction
#define LED_LINK_PIN        LED_D1      // Ethernet link (phase 2)
#define LED_SESSION_PIN     LED_D2      // console session armed
#define LED_FAULT_PIN       LED_D3      // store/config fault or safe mode
#define LED_TIMEOUT_PIN     LEDR        // last RS485 transaction timed out
// LEDG is LED_BUILTIN and is driven by initVariant() — left alone.

#define GW_BUTTON_PIN       BTN_USER    // on-board button; polarity measured at
                                        // runtime and reported as sys.btn

// ── Serial / logging ───────────────────────────────────────────────────────
#define SERIAL_BAUD             115200

// Logging is gated at RUNTIME (sys.log, default off) so a single build serves
// every deployment: with logging off the USB port carries nothing but Modbus
// and console traffic. Never persisted — see gw_config.h.
extern bool g_logEnabled;
#define LOG_SERIAL  if (!g_logEnabled) {} else Serial

// ── Factory defaults: RS485 / Modbus RTU ───────────────────────────────────
#define DEF_RS485_BAUD          9600
#define DEF_RS485_PRE_DELAY_US  10000   // µs – pre-TX delay
#define DEF_RS485_POST_DELAY_US 1000    // µs – post-TX delay
#define DEF_TIMEOUT_FIRST_BYTE_MS   300 // ms – wait for first byte from slave
#define DEF_TIMEOUT_INTER_BYTE_MS   20  // ms – inter-byte frame-gap
#define RTU_BUF_SIZE            256

// ── Factory defaults: USB bridge framing ───────────────────────────────────
#define DEF_USB_FRAME_GAP_MS    10      // ms – silence that ends one RTU frame
#define DEF_USB_FRAME_MAX_MS    100     // ms – hard cap on one frame

// ── Factory defaults: network ──────────────────────────────────────────────
// Off by default: a gateway shipped for SMT talks over USB only, and a unit
// that has never been configured should not put an address on someone's LAN.
#define DEF_NET_ENABLED         0
#define DEF_NET_IP              0xC0A800B2UL    // 192.168.0.178
#define DEF_NET_MASK            0xFFFFFF00UL    // 255.255.255.0
#define DEF_NET_GW              0xC0A80001UL    // 192.168.0.1
#define DEF_NET_DNS             0xC0A80001UL    // 192.168.0.1
#define DEF_NET_PORT            502
#define DEF_NET_LINK_TIMEOUT_MS 1500    // ms – bounded Ethernet.begin() wait

// ── NTP time recovery ──────────────────────────────────────────────────────
// After a power cut the clock (and so the scheduled resets) recovers with no
// human visit: one SNTP query to net.ntp after every link-up, re-checked
// daily. The server is an IP, never a hostname — a DNS lookup can block 15 s,
// which is a watchdog reset. 0.0.0.0 = feature off (tool-sync only, as
// before). NTP answers UTC and this firmware keeps WALL time, so time.tz_min
// (default Thailand, +420) is applied at that boundary and nowhere else.
#define DEF_NET_NTP             0UL     // NTP server IP, 0 = off
#define DEF_NET_NTP_PORT        123     // matches the Test Tool's own server
#define DEF_TIME_TZ_MIN         420     // UTC offset, minutes east
#define NTP_WAIT_MS             2000UL  // reply deadline per query
#define NTP_RETRY_MS            900000UL   // failed query: try again in 15 min
#define NTP_RESYNC_MS           86400000UL // synced: re-check daily
#define NTP_LOCAL_PORT          50123   // our end of the UDP conversation

// Lamp test (panel action 5, the yellow button's default): how long each
// status lamp stays lit before the next takes over.
#define PANEL_LAMPTEST_STEP_MS  1000UL

// ── Modbus TCP framing ─────────────────────────────────────────────────────
// The listening port itself is runtime config (net.port), not a constant.
#define TCP_BUF_SIZE        256
#define MBAP_HEADER_LEN     6
#define TIMEOUT_TCP_PAYLOAD_MS  100UL   // ms – wait for fragmented TCP payload

// RS485 switch hub (see modbus_rtu.cpp for the full model). Measured on a
// live LGS-64 cabinet: the first frame on a new channel triggers the switch
// and is always lost, and the channel stays deaf for ~2 s — repair frames at
// 0.8/1.15/1.55 s after the trigger were all eaten, the first request after
// 2.0-2.5 s always passed. The repair is therefore a settle CLOCK: spend the
// trigger, remember when the channel opens, and hold later requests in
// silence until that deadline instead of feeding the hub frames to swallow.
//
// RETRY is attempts for the crossing transaction itself: 2 means trigger
// plus one post-settle send, which only ever fires when the budget is
// raised for a patient master; against a normal 1-1.5 s master the second
// attempt cannot fit and the crossing fails in one cheap timeout.
#define DEF_HUB_RETRY        2
// Margin added past the computed deadline before transmitting, for hubs
// whose settle time wanders. Folded into the deadline, not a sleep of its
// own; the measured spread (fail at 2.03 s, pass at 2.53 s) is already
// inside DEF_HUB_SETTLE_MS, so no extra by default.
#define DEF_HUB_GAP_MS       0
// How long the hub stays deaf after the trigger frame. The single number
// that belongs to the hub hardware on site, hence runtime-tunable as
// bus.hub_settle_ms.
#define DEF_HUB_SETTLE_MS    2200
// Ceiling for one transaction (hold + send + reply), measured from its
// start. Must stay under what the master is willing to wait or the bridge
// desynchronises — which is worse than the frame the repair set out to
// save. 1400 suits the common 1.5-2 s masters; a master that waits ≥2.6 s
// can raise this past settle+2×t1 and then even the crossing transaction
// repairs itself in-line.
#define DEF_HUB_BUDGET_MS    1400

// ── Factory defaults: front-panel buttons ──────────────────────────────────
// Off until a site says otherwise: a gateway on a bench has nothing wired to
// its inputs, and a stray voltage there must not sweep somebody's cabinet.
#define DEF_PANEL_ENABLED    0
#define DEF_PANEL_CABINET    64
// Slot to slot. The bus itself costs ~100 ms a slot, so this is only extra
// breathing room; 0 runs as fast as the bus allows.
#define DEF_PANEL_STEP_MS    0
// The shelf's power drops for this long on a reset press. Long enough that
// the modules' rails actually collapse, short enough not to look like a fault.
#define DEF_PANEL_RESET_MS   1500

// ── Panel status lamps ─────────────────────────────────────────────────────
// These are mechanical relays, not LEDs on a board: a lamp that followed
// every Modbus transaction would chatter itself to death and be unbearable
// to stand next to. So traffic holds the amber lamp for a window rather than
// pulsing it, and no lamp may change state faster than the dwell — under a
// server polling steadily the amber simply stays on.
#define DEF_PANEL_LAMPS         1       // lamps follow the gateway by default
#define DEF_PANEL_LAMP_HOLD_MS  600     // traffic this recent counts as "talking"
#define DEF_PANEL_LAMP_DWELL_MS 400     // minimum time between lamp changes
// Consecutive RS485 timeouts before the bus is called dead. A hub channel
// change costs one timeout and then answers, so this sits well above the
// noise of normal operation.
#define DEF_PANEL_LAMP_DEAD      10
// What each relay output does, as the cabinet was first built: O1 carries the
// shelf's power (which is what a reset drops), and O2-O4 are the traffic
// light. All four are mapped the same way, so moving the shelf's power to
// another relay is a setting rather than a firmware change. See PanelSource
// in panel.h for the full list.
#define DEF_PANEL_OUT1       8      // SRC_SHELF
#define DEF_PANEL_OUT2       1      // SRC_READY
#define DEF_PANEL_OUT3       2      // SRC_BUSY
#define DEF_PANEL_OUT4       3      // SRC_FAULT
// Which module preset the sweeps light (coils 1010+p / 1030+p). Brightness
// and colour live in that preset's per-module config, so "the panel is too
// bright" is answered by pointing the panel at a dimmer preset.
#define DEF_PANEL_PRESET     1
// Temporary test brightness for the light sweeps: 0 leaves each module's
// own preset brightness alone; 1-100 writes the module's VOLATILE global
// brightness (reg 190) before lighting it — modules forget it at power-off,
// so a test cannot silently rewrite a site's configured look.
#define DEF_PANEL_BRIGHT     0

// ── Factory defaults: clock and scheduler ──────────────────────────────────
// Off by default. A cabinet that power-cycles itself at an hour nobody chose
// is a fault, not a feature.
#define DEF_SCHED_RESET_ENABLED 0
#define DEF_SCHED_RESET_HHMM    300     // slot 1: 03:00, as HHMM
#define DEF_SCHED_RESET_HHMM2   900     // slot 2: 09:00 — a time, not a plan;
#define DEF_SCHED_RESET_HHMM3   1500    // slot 3: 15:00   the slots ship off,
#define DEF_SCHED_RESET_HHMM4   2100    // slot 4: 21:00   so these only spare
                                        //                 typing on the first
                                        //                 tick
#define DEF_SCHED_RESET_SLOTS   0x01    // bit0=slot1..bit3=slot4; slot 1 only
#define DEF_SCHED_RESET_DAYS    0       // 0 = every day; else bit0=Sun..bit6=Sat

// ── Factory default: watchdog ──────────────────────────────────────────────
// The hardware watchdog resets the board if loop() stops running. 8 s is well
// clear of the worst legitimate stall (a cross-channel RS485 hold, ~2.2 s)
// while still bringing a wedged gateway back before anyone walks to the
// cabinet. Runtime-settable at `sys.wdt_ms`, because the worst legitimate
// stall depends on the site's wiring — the hub, and the boot-time wait for an
// Ethernet link, both live in config.
//
// The STM32's IWDG cannot be reconfigured once started, so a change takes
// effect on the next boot; the key is marked reboot-only for that reason.
#define DEF_WATCHDOG_MS         8000
#define MIN_WATCHDOG_MS         1000
#define MAX_WATCHDOG_MS         30000   // IWDG tops out near 32.7 s on the H7

// Longest DHCP wait netRuntime_begin() may add on top of net.link_timeout_ms.
// Here rather than in net_runtime.cpp because the watchdog has to be sized
// around it: this is the one stall in setup() that outlasts everything else.
#define NET_DHCP_RESPONSE_MS    4000UL

#endif // CONFIG_H
