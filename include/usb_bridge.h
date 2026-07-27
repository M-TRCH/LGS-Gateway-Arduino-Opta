#ifndef USB_BRIDGE_H
#define USB_BRIDGE_H

#include <Arduino.h>
#include "config.h"

// ── USB-RS485 bridge ───────────────────────────────────────────────────────
// Turns the Opta into a USB→RS485 Modbus RTU converter: a PC Modbus master
// sends raw RTU frames over the USB COM port and this bridge runs them on
// the RS485 bus, returning the slave reply unchanged. While the mode is
// active the COM port carries binary Modbus traffic only, so all serial
// logging is silenced via rtu_setQuiet().

// Call once when entering the mode: silences RS485 diagnostics and discards
// stale host bytes.
void usbBridge_begin();

// Call once when leaving the mode: re-enables RS485 diagnostics.
void usbBridge_end();

// Call every loop() iteration while the mode is active.
void usbBridge_update();

#endif // USB_BRIDGE_H
