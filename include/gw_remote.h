#ifndef GW_REMOTE_H
#define GW_REMOTE_H

#include <Arduino.h>

// ── Gateway-self requests over Modbus TCP (unit GW_SELF_UNIT, FC 0x41) ─────
// Everything the USB console can do, reachable through the same port 502 the
// Modbus bridge already serves — because lwIP on this board caps at 4 sockets
// and there is no room for another listener. tcp_bridge routes unit 255 here
// and the bus never sees it.
//
// Subfunctions (first PDU byte after the FC; all fields big-endian):
//   0x01 CONSOLE_EXEC  req: line (ASCII, 1..120, no "$LGS ", no CR/LF)
//   0x02 CONSOLE_READ  req: off u16
//        both resp:    total u16 · off u16 · len u8 · data[len<=200]
//   0x10 FW_BEGIN      req: size u32 · crc32 u32     resp: status u8
//   0x11 FW_DATA       req: offset u32 · data[1..240] resp: status u8 · received u32
//   0x12 FW_STATUS     req: -   resp: state u8 · received u32 · size u32 ·
//                                     ota_capable u8 · boot_ver u8
//   0x13 FW_APPLY      req: -   resp: status u8 (sent BEFORE the reset fires)
//   0x14 FW_ABORT      req: -   resp: status u8
//
// Errors are standard Modbus exceptions (FC|0x80): 01 = unknown sub, FC other
// than 0x41, or net.console=0; 03 = malformed payload; 04 = the console
// output buffer belongs to the other client slot.
//
// The console session (HELLO arming, staged edits) is the same single state
// the USB console uses — one operator, one gateway.

// Handle one PDU addressed to the gateway itself. `pdu` starts at the FC
// byte; the response PDU (which may be an exception) is written to `out`.
// Returns the response length, always >= 2.
int gwRemote_handle(const uint8_t* pdu, int pdu_len,
                    uint8_t* out, int out_max, int slot);

// Deferred work: the post-APPLY reset and the stalled-upload timeout.
// Call every loop(), like gwConsole_update().
void gwRemote_update();

#endif // GW_REMOTE_H
