#ifndef MODBUS_RTU_H
#define MODBUS_RTU_H

#include <Arduino.h>
#include <ArduinoRS485.h>
#include "config.h"

// ── Debug ──────────────────────────────────────────────────────────────────
void printHex(const char* label, const uint8_t* buf, int len);

// Suppress RS485 diagnostics on Serial while the USB-RS485 bridge owns the
// serial stream (log text would corrupt the binary RTU session).
void rtu_setQuiet(bool quiet);

// ── CRC-16/Modbus ──────────────────────────────────────────────────────────
uint16_t crc16(const uint8_t* buf, int len);
bool     verifyCRC(const uint8_t* buf, int len);

// ── RS485 Transaction ──────────────────────────────────────────────────────
// Send an RTU frame and receive the slave response.
// Handles TX-echo stripping automatically.
// @param tx     Frame bytes to transmit (must already include trailing CRC).
// @param tx_len Number of bytes to transmit.
// @param rx     Caller-allocated buffer (>= RTU_BUF_SIZE bytes).
// @return Number of response bytes received (0 = timeout).
int rtu_transact(const uint8_t* tx, int tx_len, uint8_t* rx);

// ── Modbus FC05 – Write Single Coil ───────────────────────────────────────
// @param slaveId   Modbus slave address (1–247).
// @param coilAddr  Coil address (0-based).
// @param value     true = ON, false = OFF.
// @return true on success, false on timeout / CRC error / exception.
bool writeCoil(uint8_t slaveId, uint16_t coilAddr, bool value);

#endif // MODBUS_RTU_H
