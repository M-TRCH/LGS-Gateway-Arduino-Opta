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

#endif // CONFIG_H
