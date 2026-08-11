#include "sched.h"

#include <time.h>

#include "mbed_rtc_time.h"

#include "panel.h"

static bool     _timeSet = false;
static uint8_t  _enabled = 0;
static uint16_t _hhmm[GW_SCHED_SLOTS] = { DEF_SCHED_RESET_HHMM };
static uint8_t  _slots = DEF_SCHED_RESET_SLOTS;
static uint8_t  _days = DEF_SCHED_RESET_DAYS;
static uint32_t _lastFire = 0;
// Day-of-year and minute of each slot's last firing, so a schedule fires once
// even though its minute is checked many times a second. Per slot, not shared:
// two slots set to the same minute are a configuration mistake, and one stamp
// between them would hide it by making the second slot look like a repeat.
static uint32_t _firedStamp[GW_SCHED_SLOTS];
static uint32_t _nextCheckMs = 0;

static void forgetFirings() {
    for (int s = 0; s < GW_SCHED_SLOTS; s++) _firedStamp[s] = 0xFFFFFFFFUL;
}

static const char* const DAY_NAME[7] = { "Sun", "Mon", "Tue", "Wed",
                                         "Thu", "Fri", "Sat" };

void sched_setTime(uint32_t epoch) {
    set_time((time_t)epoch);
    _timeSet = true;
    // A clock that has just moved must not fire for a minute it "missed"
    // while it was wrong, nor re-fire one it already served.
    forgetFirings();
}

bool sched_timeSet() { return _timeSet; }

uint32_t sched_now() {
    return _timeSet ? (uint32_t)time(nullptr) : 0;
}

void sched_formatNow(char* out, size_t n) {
    if (!_timeSet) { snprintf(out, n, "unset"); return; }
    const time_t t = time(nullptr);
    struct tm tmv;
    gmtime_r(&t, &tmv);             // the clock holds wall time already
    // ISO with a T, not a space: the console's key=value lines are split on
    // whitespace, and a value with a space in it arrives truncated.
    snprintf(out, n, "%04d-%02d-%02dT%02d:%02d:%02d",
             tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday,
             tmv.tm_hour, tmv.tm_min, tmv.tm_sec);
}

// Never emits a space: the console's key=value lines are split on whitespace,
// so a value with a space in it arrives at the reader truncated.
void sched_describeNext(char* out, size_t n) {
    if (!_enabled) { snprintf(out, n, "off"); return; }
    if (!_timeSet) { snprintf(out, n, "waiting_for_clock"); return; }
    char times[6 * GW_SCHED_SLOTS + 1] = "";
    size_t at = 0;
    for (int s = 0; s < GW_SCHED_SLOTS; s++) {
        if (!(_slots & (1u << s))) continue;
        at += snprintf(times + at, (at < sizeof(times)) ? sizeof(times) - at : 0,
                       "%s%02u:%02u", at ? "," : "",
                       (unsigned)(_hhmm[s] / 100), (unsigned)(_hhmm[s] % 100));
    }
    // Enabled with nothing armed is a real state and reads as a mistake, which
    // is exactly what it is — better said out loud than shown as "off".
    if (at == 0) { snprintf(out, n, "on_but_no_slots"); return; }

    char days[32] = "daily";
    if (_days != 0 && _days != 0x7F) {
        size_t d_at = 0;
        days[0] = '\0';
        for (int d = 0; d < 7; d++) {
            if (!(_days & (1u << d))) continue;
            d_at += snprintf(days + d_at,
                             (d_at < sizeof(days)) ? sizeof(days) - d_at : 0,
                             "%s%s", d_at ? "," : "", DAY_NAME[d]);
        }
    }
    snprintf(out, n, "%s@%s", times, days);
}

uint32_t sched_lastFireEpoch() { return _lastFire; }

void sched_applyConfig() {
    const GwConfig& c = gwConfig_active();
    _enabled = c.sched_reset_enabled;
    for (int s = 0; s < GW_SCHED_SLOTS; s++) _hhmm[s] = c.sched_reset_hhmm[s];
    _slots = c.sched_reset_slots;
    _days = c.sched_reset_days;
}

void sched_begin() {
    forgetFirings();
    sched_applyConfig();
}

void sched_update() {
    // A schedule has minute resolution; checking it a few times a second is
    // plenty and keeps time() off the hot path.
    const uint32_t ms = millis();
    if ((int32_t)(ms - _nextCheckMs) < 0) return;
    _nextCheckMs = ms + 250;

    if (!_enabled || !_slots || !_timeSet) return;

    const time_t t = time(nullptr);
    struct tm tmv;
    gmtime_r(&t, &tmv);

    // 0 means every day, so a map nobody edited still runs. The days apply to
    // every slot: four times a day is a rhythm, and a rhythm that changed its
    // shape from slot to slot would be unreadable at the cabinet.
    if (_days != 0 && !(_days & (1u << tmv.tm_wday))) return;

    const uint16_t nowHhmm = (uint16_t)(tmv.tm_hour * 100 + tmv.tm_min);
    const uint32_t stamp = (uint32_t)tmv.tm_yday * 1440UL
                         + (uint32_t)(tmv.tm_hour * 60 + tmv.tm_min);

    // Every slot that names this minute is marked served before anything
    // fires. Two slots set to the same time is a configuration mistake, and
    // marking only the one that won the loop would let the other fire a
    // quarter of a second later — a second power cut, not a repeat of one.
    bool due = false;
    for (int s = 0; s < GW_SCHED_SLOTS; s++) {
        if (!(_slots & (1u << s))) continue;
        if (_hhmm[s] != nowHhmm) continue;
        if (stamp == _firedStamp[s]) continue;   // already served this minute
        _firedStamp[s] = stamp;
        due = true;
    }
    if (!due) return;

    _lastFire = (uint32_t)t;
    // The same path the white button takes, so a scheduled reset and a pressed
    // one are the same event as far as everything else is concerned.
    panel_startReset();
}
