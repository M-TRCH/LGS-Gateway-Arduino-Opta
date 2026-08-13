#include "ntp.h"

#include <Ethernet.h>

#include "gw_config.h"
#include "gw_status.h"
#include "net_runtime.h"
#include "sched.h"

enum class NtpState : uint8_t { OFF, WAITING_LINK, SYNCING, OK, FAILED };

static NtpState     _state = NtpState::OFF;
static EthernetUDP* _udp = nullptr;         // exists only while a query is out
static bool         _waiting = false;
static uint32_t     _deadlineMs = 0;        // reply deadline of the query out
static uint32_t     _nextAttemptMs = 0;     // when the next query is due
static uint32_t     _lastSyncEpoch = 0;
static uint8_t      _pkt[48];

// Seconds between the NTP era (1900) and the Unix era (1970).
#define NTP_UNIX_OFFSET 2208988800UL

static void closeQuery() {
    if (_udp) {
        _udp->stop();
        delete _udp;                        // frees the core's ~1 KB buffers
        _udp = nullptr;
    }
    _waiting = false;
}

static void failQuery(uint32_t now) {
    closeQuery();
    _state = NtpState::FAILED;
    gwStatus_count(GW_NTP_FAIL);
    _nextAttemptMs = now + NTP_RETRY_MS;
}

void ntp_kick() {
    // Due immediately; a query already in flight is left to finish.
    _nextAttemptMs = millis();
}

void ntp_update() {
    const GwConfig& c = gwConfig_active();

    if (c.net_ntp == 0) {
        if (_waiting) closeQuery();
        _state = NtpState::OFF;
        return;
    }
    if (!netRuntime_isUp()) {
        // The link-up path calls ntp_kick(), so leaving _nextAttemptMs alone
        // here costs nothing — the kick will bring it forward.
        if (_waiting) closeQuery();
        _state = NtpState::WAITING_LINK;
        return;
    }

    const uint32_t now = millis();

    if (_waiting) {
        if (_udp->parsePacket() >= 48) {
            _udp->read(_pkt, sizeof(_pkt));
            const uint32_t secs1900 = ((uint32_t)_pkt[40] << 24) | ((uint32_t)_pkt[41] << 16)
                                    | ((uint32_t)_pkt[42] << 8)  |  (uint32_t)_pkt[43];
            closeQuery();
            // Server answers UTC; the clock keeps wall time — tz_min bridges
            // the two here only. Same sanity floor as the console's TIME.
            const uint32_t wall =
                (uint32_t)((int64_t)(secs1900 - NTP_UNIX_OFFSET)
                           + (int64_t)c.time_tz_min * 60);
            if (secs1900 != 0 && wall >= 1600000000UL) {
                sched_setTime(wall, TimeSource::NTP);
                _lastSyncEpoch = wall;
                _state = NtpState::OK;
                gwStatus_count(GW_NTP_OK);
                _nextAttemptMs = now + NTP_RESYNC_MS;
            } else {
                failQuery(now);
            }
        } else if ((int32_t)(now - _deadlineMs) >= 0) {
            failQuery(now);
        }
        return;
    }

    if ((int32_t)(now - _nextAttemptMs) < 0) return;

    // Launch one query. endPacket() can legitimately block up to ~1 s inside
    // the core, so the watchdog is fed right before it.
    _state = NtpState::SYNCING;
    gwStatus_watchdogKick();
    _udp = new EthernetUDP();
    if (_udp->begin(NTP_LOCAL_PORT) != 1) {
        failQuery(now);
        return;
    }
    memset(_pkt, 0, sizeof(_pkt));
    _pkt[0] = 0xE3;                         // LI unknown, version 4, mode 3 (client)
    const uint32_t ip = c.net_ntp;
    IPAddress server((uint8_t)(ip >> 24), (uint8_t)(ip >> 16),
                     (uint8_t)(ip >> 8), (uint8_t)ip);
    const bool sent = _udp->beginPacket(server, c.net_ntp_port)
                   && _udp->write(_pkt, sizeof(_pkt)) == sizeof(_pkt)
                   && _udp->endPacket();
    if (!sent) {
        failQuery(now);
        return;
    }
    _deadlineMs = millis() + NTP_WAIT_MS;
    _waiting = true;
}

const char* ntp_stateName() {
    switch (_state) {
        case NtpState::WAITING_LINK: return "waiting_link";
        case NtpState::SYNCING:      return "syncing";
        case NtpState::OK:           return "ok";
        case NtpState::FAILED:       return "failed";
        case NtpState::OFF: default: return "off";
    }
}

uint32_t ntp_lastSyncEpoch() { return _lastSyncEpoch; }
