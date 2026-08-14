#include "event_log.h"

#include <BlockDevice.h>
#include <SlicingBlockDevice.h>

#include "gw_status.h"
#include "sched.h"

// The raw QSPI window, byte offsets on the 16 MB chip. Everything below
// 14 MB belongs to a QSPIFormat partition (WiFi / OTA / kvstore / user FS)
// and 15.5-16 MB is the memory-mapped WiFi firmware — this middle strip is
// the one region no partition claims.
#define EVLOG_START     (14UL * 1024UL * 1024UL)
#define EVLOG_END       (15UL * 1024UL * 1024UL + 512UL * 1024UL)
#define EVLOG_SECTOR    4096UL
#define EVLOG_REC_SIZE  16UL
#define EVLOG_RECORDS   ((EVLOG_END - EVLOG_START) / EVLOG_REC_SIZE)   // 98,304

static_assert(sizeof(EventRecord) == EVLOG_REC_SIZE, "flash record layout");

static mbed::SlicingBlockDevice* _bd = nullptr;
static bool     _ok = false;
static uint32_t _nextSeq = 1;           // seq the NEXT record will carry

// A record's home is derived from its seq, so the head never needs storing:
// position (seq-1) % EVLOG_RECORDS, forever.
static uint64_t offsetOfSeq(uint32_t seq) {
    return (uint64_t)((seq - 1) % EVLOG_RECORDS) * EVLOG_REC_SIZE;
}

static void scanForHead() {
    // Pass 1: the first record of each sector. Seqs increase along the ring,
    // so the sector holding the newest record is the one whose first seq is
    // the largest (empty sectors read 0xFFFFFFFF and are skipped).
    uint32_t bestSeq = 0;
    uint32_t bestSector = 0;
    const uint32_t sectors = (uint32_t)((EVLOG_END - EVLOG_START) / EVLOG_SECTOR);
    for (uint32_t s = 0; s < sectors; s++) {
        uint32_t first = 0xFFFFFFFFUL;
        if (_bd->read(&first, (uint64_t)s * EVLOG_SECTOR, sizeof(first)) != 0) {
            _ok = false;
            return;
        }
        if (first != 0xFFFFFFFFUL && first != 0 && first > bestSeq) {
            bestSeq = first;
            bestSector = s;
        }
        if ((s & 0x3F) == 0) gwStatus_watchdogKick();
    }
    if (bestSeq == 0) {                 // blank window: brand-new board
        _nextSeq = 1;
        return;
    }
    // Pass 2: walk the winning sector for the last written record.
    EventRecord r;
    uint32_t last = bestSeq;
    const uint32_t perSector = EVLOG_SECTOR / EVLOG_REC_SIZE;
    for (uint32_t i = 1; i < perSector; i++) {
        const uint64_t off = (uint64_t)bestSector * EVLOG_SECTOR + i * EVLOG_REC_SIZE;
        if (_bd->read(&r, off, sizeof(r)) != 0) { _ok = false; return; }
        if (r.seq == 0xFFFFFFFFUL) break;
        last = r.seq;
    }
    _nextSeq = last + 1;
}

void eventLog_begin() {
    mbed::BlockDevice* root = mbed::BlockDevice::get_default_instance();
    if (!root || root->init() != 0) return;             // log off, boot goes on
    static mbed::SlicingBlockDevice slice(root, EVLOG_START, EVLOG_END);
    if (slice.init() != 0) return;
    _bd = &slice;
    _ok = true;
    scanForHead();
    if (_ok) {
        eventLog_note(GW_EV_BOOT, gwStatus_resetReasonCode(),
                      gwStatus_bootAttempts());
    }
}

void eventLog_note(uint8_t type, uint8_t aux, uint16_t param) {
    if (!_ok) return;

    const uint64_t off = offsetOfSeq(_nextSeq);
    // Entering a fresh sector: erase it first, so the ring eats its oldest
    // 256 records in one bite. A few tens of ms, once per 256 events.
    if (off % EVLOG_SECTOR == 0) {
        if (_bd->erase(off, EVLOG_SECTOR) != 0) { _ok = false; return; }
    }

    EventRecord r;
    r.seq     = _nextSeq;
    r.epoch   = sched_now();            // 0 while the clock is unset
    r.uptimeS = gwStatus_uptimeS();
    r.type    = type;
    r.aux     = aux;
    r.param   = param;
    if (_bd->program(&r, off, sizeof(r)) != 0) { _ok = false; return; }
    _nextSeq++;
}

bool eventLog_read(uint32_t back, EventRecord& out) {
    if (!_ok || _nextSeq <= 1) return false;
    const uint32_t newest = _nextSeq - 1;
    if (back >= newest) return false;                   // before the beginning
    if (back >= EVLOG_RECORDS) return false;            // older than the ring
    const uint32_t seq = newest - back;
    if (_bd->read(&out, offsetOfSeq(seq), sizeof(out)) != 0) return false;
    return out.seq == seq;              // torn/overwritten slot reads false
}

bool     eventLog_ok()    { return _ok; }
uint32_t eventLog_count() { return _nextSeq - 1; }

const char* eventLog_typeName(uint8_t type) {
    switch (type) {
        case GW_EV_BOOT:         return "boot";
        case GW_EV_CLOCK_SET:    return "clock_set";
        case GW_EV_LINK_UP:      return "link_up";
        case GW_EV_LINK_DOWN:    return "link_down";
        case GW_EV_TCP_ACCEPT:   return "tcp_accept";
        case GW_EV_TCP_CLOSE:    return "tcp_close";
        case GW_EV_TCP_REFUSED:  return "tcp_refused";
        case GW_EV_CFG_SAVED:    return "cfg_saved";
        case GW_EV_SCHED_FIRED:  return "sched_reset";
        case GW_EV_SWEEP:        return "panel_sweep";
        case GW_EV_STORE_ERASED: return "store_erased";
        case GW_EV_FW:           return "fw_update";
        default:                 return "unknown";
    }
}
