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
// Depot: module firmware images kept on the OTA FAT partition (4 slots, <=128 KB each) so a laptop plugged straight into
// this gateway can leave one behind and the Tool can read it back later (docs/design-module-depot.md). Never executed here.
//   0x20 DEPOT_BEGIN   req: size u32 · crc32 u32 · name[1..24 of A-Za-z0-9._-]   resp: status u8 · slot u8
//   0x21 DEPOT_DATA    req: offset u32 · data[1..240]   resp: status u8 · received u32 (CRC-checked on the last chunk)
//   0x22 DEPOT_LIST    req: slot u8   resp: slot u8 · state u8 (0 empty / 1 incomplete / 2 complete) · size u32 ·
//                                            crc32 u32 · name_len u8 · name
//   0x23 DEPOT_READ    req: slot u8 · offset u32   resp: status u8 · len u8 · data[<=200]
//   0x24 DEPOT_DELETE  req: slot u8   resp: status u8 · slot u8
// BEGIN and DELETE need the armed HELLO session; LIST and READ do not. Status adds 11 full, 12 bad name, 13 busy
// (a firmware stage is receiving), 14 not found.
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

// ── For the background OTA runner (ota_runner) ───────────────────────────
// The OTA FAT partition is shared with the depot and the stored OTA results. gwRemote_fsMount() makes sure it is mounted
// (and restarts the idle timer); gwRemote_fsHold(true) keeps it mounted until released.
bool gwRemote_fsMount();
void gwRemote_fsHold(bool hold);
// Copy a complete depot image into `buf` (capacity `cap`). Returns 0 ok, 1 slot not complete, 2 too big, 3 storage error.
int  gwRemote_depotImage(uint8_t slot, uint8_t* buf, uint32_t cap, uint32_t& size, uint32_t& crc, char* name25, uint8_t& nameLen);

#endif // GW_REMOTE_H
