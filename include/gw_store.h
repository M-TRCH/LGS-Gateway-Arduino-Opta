#ifndef GW_STORE_H
#define GW_STORE_H

#include <Arduino.h>

struct GwConfig;

// ── Persistent settings storage ────────────────────────────────────────────
// mbed::TDBStore over MBRBlockDevice partition 3 — the 1 MB region Arduino's
// QSPIFormat.ino reserves as "kvstore". 4 KB erase unit, ~100k cycles.
//
// Deliberately NOT the kv_set/kv_get global API: on the Opta the default KV
// configuration resolves to TDB_EXTERNAL over the WHOLE 16 MB QSPI, which
// would sit on top of the MBR, the WiFi firmware and the OTA partition.
//
// Rules this module obeys:
//   * a failure here is never fatal — the caller falls back to defaults and
//     the USB bridge keeps running;
//   * it never formats or partitions anything;
//   * a virgin unit is not auto-written (no flash wear before the first SAVE);
//   * a corrupt record is never overwritten until an explicit save.

enum class GwStoreStatus : uint8_t { OK, NO_RECORD, CORRUPT, UNAVAILABLE };

// Mounts the block device and the key-value store. Safe to call once at boot;
// returns UNAVAILABLE when partition 3 is missing or cannot be opened.
GwStoreStatus gwStore_begin();

GwStoreStatus gwStore_load(GwConfig& out);
bool          gwStore_save(const GwConfig& in);
bool          gwStore_erase();

bool        gwStore_available();
const char* gwStore_lastError();

#endif // GW_STORE_H
