#ifndef CONFIG_H
#define CONFIG_H

#include <Arduino.h>

// Values here are compile-time DEFAULTS. Anything a deployment may need to
// change lives in GwConfig (gw_config.h) and is settable at runtime over the
// USB console; these constants only seed the factory defaults.

// ── Hardware Pins ──────────────────────────────────────────────────────────
#define MODULE_RELAY_PIN    D0
#define LED_RELAY_PIN       D1
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

#endif // CONFIG_H
