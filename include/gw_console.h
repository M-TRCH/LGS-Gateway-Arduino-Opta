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

// ── Remote entry (console over TCP, gw_remote.cpp) ─────────────────────────
// Where a console command's output goes. The default (USB) sink prints to
// Serial; a remote caller supplies its own and collects the lines.
typedef void (*GwEmitFn)(const char* line);

// Run ONE command body ("INFO", "SET key value", ...) with every emitted line
// routed to `sink` for the synchronous duration of the call. `body` is
// tokenised in place (strtok) — the caller passes its own mutable copy.
//
// The session (HELLO arming, staged edits) is ONE state shared by USB and
// remote callers alike: this gateway assumes a single operator, and the
// remote path inherits exactly the protections USB has, no more.
void gwConsole_execute(char* body, GwEmitFn sink);

// Push the armed-session deadline out, exactly as receiving a command does.
// For the firmware-upload subfunctions, whose frames are not console verbs
// but must keep the session alive across a long transfer.
void gwConsole_refreshSession();

#endif // GW_CONSOLE_H
