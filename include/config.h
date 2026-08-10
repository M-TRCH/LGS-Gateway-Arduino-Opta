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
// O2 used to be a second power relay for the LED rail (LED_RELAY_PIN). It is
// the green lamp now — one output cannot be both, and a reset that dropped
// the green lamp along with the shelf was only ever incidentally right.
#define MODULE_RELAY_PIN    D0          // O1 — shelf power, dropped on reset
#define PANEL_LAMP_GREEN    D1          // O2 — ready
#define PANEL_LAMP_YELLOW   D2          // O3 — talking to the cabinet
#define PANEL_LAMP_RED      D3          // O4 — not ready / resetting
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

#endif // CONFIG_H
