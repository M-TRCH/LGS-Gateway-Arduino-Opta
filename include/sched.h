#ifndef SCHED_H
#define SCHED_H

#include <Arduino.h>
#include "gw_config.h"

/*  Clock and scheduler.
 *
 *  The Opta has an RTC but nothing to keep it running with the power off, so
 *  the time is lost on every power cut — exactly the event a scheduled reset
 *  exists to recover from. That shapes everything here:
 *
 *    - the clock is UNSET until somebody sets it, and says so;
 *    - nothing is ever scheduled while it is unset, because a gateway that
 *      booted believing it was 1 January 1970 would fire the moment its
 *      target time came round in that fiction;
 *    - the Test Tool sets it whenever it connects, so in practice it is set
 *      within seconds of a power-up.
 *
 *  The clock keeps WALL time — the time on the wall in front of the cabinet —
 *  not UTC. There is no timezone anywhere in this firmware, and a schedule
 *  that says 03:00 means the 03:00 the pharmacist would recognise.
 */

void sched_begin();
void sched_update();
void sched_applyConfig();           // called by gw_config on load and save

// Set the wall clock. `epoch` is seconds since 1970 in LOCAL time.
void sched_setTime(uint32_t epoch);
bool sched_timeSet();               // false until somebody sets it
uint32_t sched_now();               // 0 when unset

// "2026-08-10 15:31:07", or "unset"
void sched_formatNow(char* out, size_t n);
// "03:00 daily", "03:00 Mon,Wed", "off", or "waiting for the time"
void sched_describeNext(char* out, size_t n);

uint32_t sched_lastFireEpoch();     // 0 = has not fired since boot

#endif // SCHED_H
