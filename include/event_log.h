#ifndef EVENT_LOG_H
#define EVENT_LOG_H

#include <Arduino.h>

/*  Event log: a ring of fixed 16-byte records on the QSPI flash, so "what
 *  happened at the cabinet last night" has an answer months later — boots
 *  (with their cause), link drops, clock sets, config saves, scheduled
 *  resets, TCP clients coming and going.
 *
 *  Where it lives: the RAW window at 14–15.5 MB of the 16 MB QSPI, which is
 *  OUTSIDE every partition Arduino's QSPIFormat creates — partition 4 is a
 *  LittleFS/PLC-runtime area and gets reformatted on re-provisioning, this
 *  window does not (only QSPIFormat's optional full-chip erase touches it).
 *
 *  ~98,000 records; at tens of events a day that is decades. When full the
 *  oldest sector is erased and overwritten — the ring never fills up.
 *
 *  Timestamps: `epoch` is the wall clock at write time and 0 while the clock
 *  is unset (right after a power cut). `uptimeS` always runs, so the events
 *  of one boot stay ordered, and the clock_set event that follows lets a
 *  reader anchor them in wall time after the fact.
 *
 *  Failure contract mirrors gw_store: any storage error turns the log off
 *  for this boot (INFO log.state=off_error) and costs nothing else.
 */

enum GwEvent : uint8_t {
    GW_EV_BOOT        = 1,  // aux = reset-reason code, param = boot attempts
    GW_EV_CLOCK_SET   = 2,  // aux = 1 tool / 2 ntp / 3 restored from RTC, param = |jump| s
                            // (clamped 65535), epoch = the new wall time
    GW_EV_LINK_UP     = 3,  // param = our IP's last octet
    GW_EV_LINK_DOWN   = 4,
    GW_EV_TCP_ACCEPT  = 5,  // aux = slot, param = client IP's last octet
    GW_EV_TCP_CLOSE   = 6,  // aux = slot
    GW_EV_TCP_REFUSED = 7,  // param = client IP's last octet (both slots busy)
    GW_EV_CFG_SAVED   = 8,
    GW_EV_SCHED_FIRED = 9,
    GW_EV_SWEEP       = 10, // aux = PanelAction, param = button 1-5 (manual only —
                            // a scheduled reset is event 9)
    GW_EV_STORE_ERASED = 11, // aux = 1 button-hold erase / 2 forced defaults
    GW_EV_FW           = 12, // network firmware update: aux = 1 stage begun /
                             // 2 apply ok (reset follows) / 3 CRC mismatch /
                             // 4 depot image stored / 5 depot image deleted,
                             // param = image size in KB
    GW_EV_BUS_QUIET    = 13, // the RS485 side went silent en masse (aux = 1)
                             // and came back (aux = 2, param = seconds down).
                             // A cabinet losing power looks exactly like this
                             // from here, and this is the only witness that
                             // survives with no PC watching.
    GW_EV_OUT_FIXED    = 14, // a relay output had drifted from its commanded
                             // level and was re-asserted (aux = output 1-4,
                             // param = the level it was put back to)
    // ---- v1.14.0: what a collector needs to answer "who did what" ----------
    GW_EV_TCP_PEER     = 15, // companion of TCP_ACCEPT, written right after it:
                             // the client's first three octets, so the pair
                             // carries the whole address (a /24 last octet alone
                             // is ambiguous behind a router): aux = octet 3,
                             // param = octet 1 << 8 | octet 2
    GW_EV_COIL         = 16, // a Modbus TCP client wrote a coil (FC 5) through
                             // the bridge: aux = unit id (0 = broadcast),
                             // param = coil address | on << 15 | no_reply << 14.
                             // Addresses stay below 16384, so the flags are free.
                             // Rate limited (see eventLog_noteCoil)
    GW_EV_COIL_DROPPED = 17, // coil events suppressed by the rate limit since
                             // the last one of these: param = count (clamped)
    GW_EV_HEAL         = 18, // RS485 first-try misses the gateway repaired since
                             // the last one of these (every 10 minutes, only when
                             // there were any): param = repairs that worked,
                             // aux = repairs that did not (clamped 255)
    // ---- v1.14.0: the gateway runs the module OTA itself (ota_runner) ------
    GW_EV_OTA          = 19  // aux = 1 started (param = modules) / 2 a group
                             // staged (param = channel << 8 | modules verified)
                             // / 3 a module left the session and goes round again
                             // (param = id) / 4 a module applied (param = id << 8
                             // | 1 if the firmware changed) / 5 finished ok /
                             // 6 finished with failures (param = failed) /
                             // 7 aborted (param = failed)
};

struct EventRecord {        // exactly 16 bytes on flash, written once
    uint32_t seq;           // 1,2,3...; 0xFFFFFFFF = empty slot
    uint32_t epoch;         // wall clock, 0 = unset
    uint32_t uptimeS;
    uint8_t  type;          // GwEvent
    uint8_t  aux;
    uint16_t param;
};

// Mount the window and find the ring's head. Call after the watchdog is
// running (the boot scan reads 1.5 MB) and after gwStatus_begin() (the OTP
// MAC read must precede any QSPI mount). Writes the boot event itself.
void eventLog_begin();

// Append one record. No-op (and free) when the log is off.
void eventLog_note(uint8_t type, uint8_t aux, uint16_t param);

// Read the record `back` places behind the newest (0 = newest).
// false = no such record (younger than the ring or before the beginning).
bool eventLog_read(uint32_t back, EventRecord& out);

// Oldest seq that can still be read (0 = log empty or off). Records before it
// were overwritten; a collector asking for older ones is told about the gap.
uint32_t eventLog_oldest();

// A coil write seen on the TCP bridge. Rate limited: a sustained burst above
// COIL_EV_PER_S (a pharmacy server lights thousands of windows a day, a
// misbehaving client could do thousands a minute) is counted, not stored, and
// summarised by one GW_EV_COIL_DROPPED per minute. Flash endurance and the
// ring's depth are what this protects.
void eventLog_noteCoil(uint8_t unit, uint16_t addr, bool on, bool noReply);

// Housekeeping tick, from loop(): the minute-boundary drop summary and the
// 10-minute heal summary. Cheap when nothing is due.
void eventLog_update();

bool        eventLog_ok();
uint32_t    eventLog_count();           // events ever written (= newest seq)
const char* eventLog_typeName(uint8_t type);  // console-safe, no spaces

#endif // EVENT_LOG_H
