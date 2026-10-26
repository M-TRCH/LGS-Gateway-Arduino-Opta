#ifndef MODBUS_RTU_H
#define MODBUS_RTU_H

#include <Arduino.h>
#include <ArduinoRS485.h>
#include "config.h"
#include "gw_config.h"

// ── Debug ──────────────────────────────────────────────────────────────────
// Silent unless sys.log is on (see LOG_SERIAL in config.h).
void printHex(const char* label, const uint8_t* buf, int len);

// ── CRC-16/Modbus ──────────────────────────────────────────────────────────
uint16_t crc16(const uint8_t* buf, int len);
bool     verifyCRC(const uint8_t* buf, int len);

// ── Runtime timing (applied from GwConfig) ─────────────────────────────────
void rtu_setTimeouts(uint16_t firstByteMs, uint16_t interByteMs);

// ── RS485 Transaction ──────────────────────────────────────────────────────
// Send an RTU frame and receive the slave response.
// Handles TX-echo stripping automatically.
// @param tx     Frame bytes to transmit (must already include trailing CRC).
// @param tx_len Number of bytes to transmit.
// @param rx     Caller-allocated buffer (>= RTU_BUF_SIZE bytes).
// @param flags  RTU_F_* below; 0 (the default) is the plain transaction, which
//               is exactly what the USB bridge uses — a PC master does its own
//               timing, so it never gets an extra frame or a delayed reply.
// @return Number of response bytes received (0 = timeout).
int rtu_transact(const uint8_t* tx, int tx_len, uint8_t* rx, uint8_t flags = 0);

// After a transaction that got NO reply (and only when the hub is not
// mid-settle — the crossing trigger frame is lost by design):
//   RTU_F_RESYNC  send one broadcast read so every module is back in phase
//   RTU_F_RETRY   then ask once more, if this id has answered since boot and
//                 the whole transaction still fits the budget
// The retry is safe for writes: coil/register sets are idempotent.
#define RTU_F_RESYNC 0x01
#define RTU_F_RETRY  0x02
#define RTU_F_HEAL   (RTU_F_RESYNC | RTU_F_RETRY)

// Volatile kill switch (`$LGS RESYNC 0|1`), default on at every boot. Not in
// GwConfig on purpose: a new stored key is a schema bump, which wipes a unit.
void rtu_setHeal(bool on);
bool rtu_healEnabled();

// Observability, printed by $LGS INFO as rtu.*:
//   resync      broadcasts sent
//   retry_ok    transactions that failed first and were answered by the retry
//   retry_fail  retries that were not answered either
// cnt.rs485_timeout stays the FINAL outcome, so first-try misses are
// cnt.rs485_timeout + rtu.retry_ok.
uint32_t rtu_resyncCount();
uint32_t rtu_retryOk();
uint32_t rtu_retryFail();
// Replies that came back but were not an answer: bad_crc = right unit id, wrong
// CRC; bad_other = runts and frames from another unit id. A cold, flaky or
// noisy module shows up here when it answers garbled; one that stays silent
// shows up only in cnt.rs485_timeout.
uint32_t rtu_badCrc();
uint32_t rtu_badOther();

/*  Teach the RTU layer the RS485 hub wiring. `map` is GW_HUB_MAX_ROWS bytes,
 *  row 1..N -> channel 1..8 (0 = not behind the hub). All zeros disables the
 *  hub handling entirely. Called by gw_config on load and save. */
/*  Observability, printed by $LGS INFO:
 *    cross    transactions that changed hub channel (each costs its trigger)
 *    extra    additional frames spent on crossing transactions (in-line saves)
 *    wait_ms  total time requests were held for the settle deadline
 *    skip     requests failed WITHOUT touching the bus because the hold did
 *             not fit bus.hub_budget_ms — a master timing out faster than the
 *             hub settles shows up here. */
uint32_t rtu_hubCross();
uint32_t rtu_hubExtra();
uint32_t rtu_hubWaitMs();
uint32_t rtu_hubSkip();

void rtu_setHub(const uint8_t* map, uint8_t retry, uint16_t gapMs,
                uint16_t settleMs, uint16_t budgetMs);

// ── RS485 Send-only (Modbus broadcast, slave address 0) ───────────────────
// Transmits and returns as soon as the bytes are on the wire — a broadcast is
// never answered, so waiting out the first-byte timeout would only stall the
// bridge and let the host's following frames pile up behind it.
// Any TX echo left by the transceiver is discarded by the next transaction's
// pre-TX flush.
void rtu_send(const uint8_t* tx, int tx_len);

#endif // MODBUS_RTU_H
