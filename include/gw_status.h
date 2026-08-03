#ifndef GW_STATUS_H
#define GW_STATUS_H

#include <Arduino.h>
#include "config.h"

// ── Gateway status: counters, indicators, boot health ──────────────────────
// Answers the two questions every "it doesn't work" call starts with: is the
// gateway seeing frames at all, and is anything on RS485 answering?

enum GwCounter {
    GW_USB_OK = 0,        // valid Modbus frames received from the USB host
    GW_USB_DROP,          // frames dropped (too short / bad CRC / console text)
    GW_TCP_OK,            // valid frames received from a TCP client
    GW_RS485_OK,          // RS485 transactions that got a reply
    GW_RS485_TIMEOUT,     // RS485 transactions with no reply
    GW_CFG_CMD,           // console commands served
    GW_COUNTER_N
};

// Call first in setup(): latches the reset reason and bumps the boot-attempt
// counter held in the RTC backup domain (survives a warm/watchdog reset).
void gwStatus_begin();

// Counters
void     gwStatus_count(GwCounter c);
void     gwStatus_countRtu(bool replied, uint32_t rttMs);
uint32_t gwStatus_get(GwCounter c);
void     gwStatus_resetCounters();
uint16_t gwStatus_lastRttMs();
uint16_t gwStatus_maxRttMs();
uint16_t gwStatus_consecutiveTimeouts();

// Indicators (non-blocking; call gwStatus_update() every loop)
void gwStatus_pulseRs485();               // brief LED flash per transaction
void gwStatus_setSessionArmed(bool on);   // console session indicator
void gwStatus_setFault(bool on);          // store/config fault or safe mode
void gwStatus_update();

// Boot health
uint8_t     gwStatus_bootAttempts();
bool        gwStatus_safeMode();
void        gwStatus_setSafeMode(bool on);
void        gwStatus_markHealthy();       // clears the boot-attempt counter
const char* gwStatus_resetReason();
uint32_t    gwStatus_uptimeS();

// Board identity — the OTP MAC must be cached before any QSPI block device is
// mounted, so gwStatus_begin() reads it up front. When the OTP holds nothing
// usable the reported address is a placeholder: gwStatus_macValid() is false
// and the network hands begin() a nullptr so mbed derives its own instead.
const uint8_t* gwStatus_mac();
bool           gwStatus_macValid();
void           gwStatus_setMac(const uint8_t* mac);   // adopt the interface's
void           gwStatus_serialHex(char* out, size_t n);

// Raw level of the on-board button, so its polarity can be measured on the
// bench (reported as sys.btn) before any logic depends on it.
int gwStatus_buttonRaw();

#endif // GW_STATUS_H
