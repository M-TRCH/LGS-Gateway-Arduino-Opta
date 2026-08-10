#include "sched.h"

#include <time.h>

#include "mbed_rtc_time.h"

#include "panel.h"

static bool     _timeSet = false;
static uint8_t  _enabled = 0;
static uint16_t _hhmm = DEF_SCHED_RESET_HHMM;
static uint8_t  _days = DEF_SCHED_RESET_DAYS;
static uint32_t _lastFire = 0;
// Day-of-year and minute of the last firing, so a schedule fires once even
// though its minute is checked many times a second.
static uint32_t _firedStamp = 0xFFFFFFFFUL;
static uint32_t _nextCheckMs = 0;

static const char* const DAY_NAME[7] = { "Sun", "Mon", "Tue", "Wed",
                                         "Thu", "Fri", "Sat" };

void sched_setTime(uint32_t epoch) {
    set_time((time_t)epoch);
    _timeSet = true;
    // A clock that has just moved must not fire for a minute it "missed"
    // while it was wrong, nor re-fire one it already served.
    _firedStamp = 0xFFFFFFFFUL;
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

void sched_describeNext(char* out, size_t n) {
    if (!_enabled) { snprintf(out, n, "off"); return; }
    if (!_timeSet) { snprintf(out, n, "waiting for the time"); return; }
    char days[32] = "daily";
    if (_days != 0 && _days != 0x7F) {
        size_t at = 0;
        days[0] = '\0';
        for (int d = 0; d < 7; d++) {
            if (!(_days & (1u << d))) continue;
            at += snprintf(days + at, (at < sizeof(days)) ? sizeof(days) - at : 0,
                           "%s%s", at ? "," : "", DAY_NAME[d]);
        }
    }
    snprintf(out, n, "%02u:%02u %s", (unsigned)(_hhmm / 100),
             (unsigned)(_hhmm % 100), days);
}

uint32_t sched_lastFireEpoch() { return _lastFire; }

void sched_applyConfig() {
    const GwConfig& c = gwConfig_active();
    _enabled = c.sched_reset_enabled;
    _hhmm = c.sched_reset_hhmm;
    _days = c.sched_reset_days;
}

void sched_begin() {
    sched_applyConfig();
}

void sched_update() {
    // A schedule has minute resolution; checking it a few times a second is
    // plenty and keeps time() off the hot path.
    const uint32_t ms = millis();
    if ((int32_t)(ms - _nextCheckMs) < 0) return;
    _nextCheckMs = ms + 250;

    if (!_enabled || !_timeSet) return;

    const time_t t = time(nullptr);
    struct tm tmv;
    gmtime_r(&t, &tmv);

    const uint16_t nowHhmm = (uint16_t)(tmv.tm_hour * 100 + tmv.tm_min);
    if (nowHhmm != _hhmm) return;
    // 0 means every day, so a map nobody edited still runs.
    if (_days != 0 && !(_days & (1u << tmv.tm_wday))) return;

    const uint32_t stamp = (uint32_t)tmv.tm_yday * 1440UL
                         + (uint32_t)(tmv.tm_hour * 60 + tmv.tm_min);
    if (stamp == _firedStamp) return;       // already served this minute
    _firedStamp = stamp;
    _lastFire = (uint32_t)t;

    // The same path the white button takes, so a scheduled reset and a
    // pressed one are the same event as far as everything else is concerned.
    panel_startReset();
}
