#ifndef MODBUS_RTU_H
#define MODBUS_RTU_H

#include <Arduino.h>
#include <ArduinoRS485.h>
#include "config.h"

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
// @return Number of response bytes received (0 = timeout).
int rtu_transact(const uint8_t* tx, int tx_len, uint8_t* rx);

// ── RS485 Send-only (Modbus broadcast, slave address 0) ───────────────────
// Transmits and returns as soon as the bytes are on the wire — a broadcast is
// never answered, so waiting out the first-byte timeout would only stall the
// bridge and let the host's following frames pile up behind it.
// Any TX echo left by the transceiver is discarded by the next transaction's
// pre-TX flush.
void rtu_send(const uint8_t* tx, int tx_len);

#endif // MODBUS_RTU_H
