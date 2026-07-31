#ifndef GW_CONSOLE_H
#define GW_CONSOLE_H

#include <Arduino.h>

// ── $LGS text console ──────────────────────────────────────────────────────
// Configuration travels as ASCII lines on the same USB CDC the Modbus bridge
// uses. It is fed ONLY with bytes usb_bridge already rejected as Modbus, so a
// well-formed RTU frame can never reach this parser.
//
//   request  := "$LGS" SP VERB (SP ARG)* (CR | LF | CRLF)
//   response := "#DATA ..."*  then exactly one "#OK ..." or "#ERR ..."
//
// The magic is five bytes ("$LGS ") on purpose: a bare '$' is 0x24 = 36, which
// is a perfectly valid LGS slave address (row 3, column 6).
//
// Nothing is written to the port unless a complete, valid command was parsed.

void gwConsole_begin();

// Offer bytes rejected by the Modbus path. Keeps line state across calls, so
// a line typed by hand (one byte per call) assembles correctly.
void gwConsole_feed(const uint8_t* buf, int len);

// Line/session timeouts and the deferred reboot. Call every loop().
void gwConsole_update();

bool gwConsole_armed();

#endif // GW_CONSOLE_H
