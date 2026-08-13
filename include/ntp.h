#ifndef NTP_H
#define NTP_H

#include <Arduino.h>

/*  SNTP client: recover the wall clock after a power cut with no human visit.
 *
 *  One 48-byte query to `net.ntp` (an IP, never a hostname — DNS can block
 *  past the watchdog) fired after every link-up and re-checked daily; a
 *  failure retries every 15 minutes and stays silent otherwise — the Test
 *  Tool's connect-time sync keeps working exactly as before, this only adds
 *  a path that needs nobody at the site.
 *
 *  The UDP socket is transient (open → query → close): lwIP on this board
 *  allows 4 sockets total and the TCP bridge's listener + two client slots
 *  hold three of them.
 *
 *  NTP answers UTC; the clock keeps wall time. `time.tz_min` bridges the
 *  two here and nowhere else — `$LGS TIME` still hands over wall time raw.
 */

// Schedule a sync attempt soon (link-up, config save). Safe anytime; a
// disabled or link-down NTP simply ignores it at update time.
void ntp_kick();

// Poll from loop(). Non-blocking except the ~1 s worst case inside the
// core's endPacket(); the watchdog is fed right before that.
void ntp_update();

const char* ntp_stateName();     // off|waiting_link|syncing|ok|failed
uint32_t    ntp_lastSyncEpoch(); // wall epoch of the last success, 0 = never

#endif // NTP_H
