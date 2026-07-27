#ifndef CONFIG_H
#define CONFIG_H

#include <Arduino.h>

// ── Hardware Pins ──────────────────────────────────────────────────────────
#define MODULE_RELAY_PIN    D0
#define LED_RELAY_PIN       D1
#define SW_R_PIN            A0
#define SW_G_PIN            A1
#define SW_B_PIN            A2
#define SW_Y_PIN            A3
#define SW_W_PIN            A4

// ── Network ────────────────────────────────────────────────────────────────
#define NET_MAC_ADDRESS     { 0xDE, 0xAD, 0xBE, 0xEF, 0xFE, 0xED }
#define NET_STATIC_IP       192, 168, 0, 178    // octets for the IPAddress ctor

// ── Operating mode ─────────────────────────────────────────────────────────
// The external panel buttons are temporarily disabled; the operating mode is
// fixed here at build time — change USB_BRIDGE_ON_BOOT, rebuild, reflash.
#define PANEL_BUTTONS_ENABLED   0   // 0 = ignore external buttons (temporary)
#define USB_BRIDGE_ON_BOOT      0   // 0 = Modbus TCP gateway, 1 = USB-RS485 bridge

// ── Buttons ────────────────────────────────────────────────────────────────
#define BTN_DEBOUNCE_MS     50      // ms – press must persist across this gap

// ── USB-RS485 bridge ───────────────────────────────────────────────────────
#define USB_MODE_LED_PIN    LED_USER  // blue front LED: ON = USB-RS485 bridge mode
#define USB_FRAME_GAP_MS    10UL      // ms – silence that ends one RTU frame from the USB host
#define USB_FRAME_MAX_MS    100UL     // ms – hard cap on accumulating a single frame

// ── Serial ─────────────────────────────────────────────────────────────────
#define SERIAL_BAUD             115200

// ── RS485 / Modbus RTU ─────────────────────────────────────────────────────
#define RS485_BAUD              9600
#define RS485_PRE_DELAY_US      10000   // µs – pre-TX delay
#define RS485_POST_DELAY_US     1000    // µs – post-TX delay
#define RTU_BUF_SIZE            256
#define TIMEOUT_FIRST_BYTE_MS   300UL   // ms – wait for first byte from slave
#define TIMEOUT_INTER_BYTE_MS   20UL    // ms – inter-byte frame-gap

// ── Modbus TCP ─────────────────────────────────────────────────────────────
#define MODBUS_TCP_PORT     502
#define TCP_BUF_SIZE        256
#define MBAP_HEADER_LEN     6
#define TIMEOUT_TCP_PAYLOAD_MS  100UL   // ms – wait for fragmented TCP payload

// ── System ─────────────────────────────────────────────────────────────────
#define RESET_RELAY_SETTLE_MS   3000    // ms – relays LOW settle before MCU reset
#define STARTUP_SWEEP_DELAY_MS  2000    // ms – pause before startup coil sweep

// ── Self-test ──────────────────────────────────────────────────────────────
#define SELFTEST_ROWS           6       // module grid rows
#define SELFTEST_COLS           4       // module grid columns
#define SELFTEST_COIL_PRIMARY   1004    // coil toggled by the sweep test
#define SELFTEST_COIL_EXTENDED  1024    // coil set ON by the extended test
#define SELFTEST_SWEEP_STEP_MS  200     // ms – ON/OFF dwell during sweep
#define SELFTEST_EXT_ON_MS      2000    // ms – extended test ON dwell
#define SELFTEST_EXT_OFF_MS     1000    // ms – extended test OFF dwell

#endif // CONFIG_H
