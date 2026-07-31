#ifndef USB_BRIDGE_H
#define USB_BRIDGE_H

#include <Arduino.h>
#include "config.h"

// ── USB-RS485 bridge ───────────────────────────────────────────────────────
// Turns the Opta into a USB→RS485 Modbus RTU converter: a PC Modbus master
// sends raw RTU frames over the USB COM port and this bridge runs them on the
// RS485 bus, returning the slave reply unchanged. The port carries binary
// Modbus traffic, so logging is off by default (sys.log).
//
// Bytes that are NOT a valid Modbus frame were always discarded here; they are
// now offered to the text console (gw_console) instead, which is what makes
// `$LGS ...` configuration possible on the same wire without touching the
// Modbus path.

// Call once at boot: discards stale host bytes.
void usbBridge_begin();

// Frame accumulation timing, applied from GwConfig.
void usbBridge_setFraming(uint16_t gapMs, uint16_t maxMs);

// Call every loop() iteration.
void usbBridge_update();

#endif // USB_BRIDGE_H
