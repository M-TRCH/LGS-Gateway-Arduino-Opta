#include "gw_store.h"

#include <BlockDevice.h>
#include <MBRBlockDevice.h>
#include <TDBStore.h>

#include "gw_config.h"

#define GW_BLOB_MAGIC   0x4353474CUL      // 'L','G','S','C' little-endian
// Bump whenever a field's MEANING changes without the struct changing size.
// v2: panel_lamp_out[3] (which output a colour is on) became panel_out[3]
// (what each output follows) — same three bytes, different question, and a
// stored 2,3,4 silently read back as busy/fault/link.
// v3: the scheduler's three fields were appended.
// v4: panel_out grew from 3 outputs to 4 — output 1 (the shelf's power)
// joined the same mapping, so every stored index shifted by one.
// v5: the schedule became four slots and the watchdog period became config.
// v6: panel_shape was appended — the sweep shape for a non-catalogue cabinet.
// v7: panel_preset was inserted — which module preset the sweeps fire.
// v8: panel_bright was inserted — the sweeps' temporary test brightness.
// v9: panel_step_ms became three paces, one per sweep kind (on/off/unlock).
// v10: net_ntp + net_ntp_port + time_tz_min were appended — NTP recovery.
// v11: net_console appended — the TCP in-band console gate.
#define GW_BLOB_SCHEMA  11
#define GW_KVSTORE_PARTITION 3
#define GW_KEY          "gwcfg"

struct GwBlob {
    uint32_t magic;
    uint16_t schema;
    uint16_t size;          // sizeof(GwConfig): rejects a struct-layout change
    uint16_t flags;
    uint16_t pad;
    GwConfig cfg;
    uint32_t crc32;
} __attribute__((packed));

static mbed::BlockDevice*    _root  = nullptr;
static mbed::MBRBlockDevice* _part  = nullptr;
static mbed::TDBStore*       _store = nullptr;
static bool        _ready = false;
static const char* _lastError = "not_initialised";

// CRC-32/ISO-HDLC over everything ahead of the crc32 field.
static uint32_t crc32_of(const uint8_t* data, size_t len) {
    uint32_t crc = 0xFFFFFFFFUL;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int b = 0; b < 8; b++) {
            crc = (crc & 1) ? ((crc >> 1) ^ 0xEDB88320UL) : (crc >> 1);
        }
    }
    return ~crc;
}

GwStoreStatus gwStore_begin() {
    if (_ready) return GwStoreStatus::OK;

    _root = mbed::BlockDevice::get_default_instance();
    if (!_root || _root->init() != 0) {
        _lastError = "no_block_device";
        return GwStoreStatus::UNAVAILABLE;
    }

    // Partition 3 only. If it is absent this unit has not been provisioned
    // with Arduino's QSPIFormat sketch — report it, never create it.
    _part = new mbed::MBRBlockDevice(_root, GW_KVSTORE_PARTITION);
    if (!_part || _part->init() != 0) {
        _lastError = "kvstore_partition_3_missing";
        delete _part; _part = nullptr;
        return GwStoreStatus::UNAVAILABLE;
    }

    _store = new mbed::TDBStore(_part);
    if (!_store || _store->init() != MBED_SUCCESS) {
        _lastError = "tdbstore_init_failed";
        delete _store; _store = nullptr;
        _part->deinit();
        delete _part; _part = nullptr;
        return GwStoreStatus::UNAVAILABLE;
    }

    _ready = true;
    _lastError = "";
    return GwStoreStatus::OK;
}

bool        gwStore_available() { return _ready; }
const char* gwStore_lastError() { return _lastError; }

GwStoreStatus gwStore_load(GwConfig& out) {
    if (!_ready && gwStore_begin() != GwStoreStatus::OK) return GwStoreStatus::UNAVAILABLE;

    GwBlob blob;
    size_t actual = 0;
    int rc = _store->get(GW_KEY, &blob, sizeof(blob), &actual);
    if (rc == MBED_ERROR_ITEM_NOT_FOUND) {
        _lastError = "";
        return GwStoreStatus::NO_RECORD;
    }
    if (rc != MBED_SUCCESS || actual != sizeof(blob)) {
        _lastError = "record_unreadable";
        return GwStoreStatus::CORRUPT;
    }
    if (blob.magic != GW_BLOB_MAGIC || blob.size != sizeof(GwConfig) ||
        blob.schema != GW_BLOB_SCHEMA) {
        _lastError = "magic/size/schema_mismatch";
        return GwStoreStatus::CORRUPT;
    }
    uint32_t want = crc32_of((const uint8_t*)&blob, offsetof(GwBlob, crc32));
    if (want != blob.crc32) {
        _lastError = "crc_mismatch";
        return GwStoreStatus::CORRUPT;
    }
    if (blob.schema > GW_BLOB_SCHEMA) {
        _lastError = "schema_newer_than_firmware";
        return GwStoreStatus::CORRUPT;
    }

    out = blob.cfg;
    _lastError = "";
    return GwStoreStatus::OK;
}

bool gwStore_save(const GwConfig& in) {
    if (!_ready && gwStore_begin() != GwStoreStatus::OK) return false;

    GwBlob blob;
    memset(&blob, 0, sizeof(blob));
    blob.magic  = GW_BLOB_MAGIC;
    blob.schema = GW_BLOB_SCHEMA;
    blob.size   = sizeof(GwConfig);
    blob.cfg    = in;
    blob.crc32  = crc32_of((const uint8_t*)&blob, offsetof(GwBlob, crc32));

    int rc = _store->set(GW_KEY, &blob, sizeof(blob), 0);
    if (rc != MBED_SUCCESS) {
        _lastError = "write_failed";
        return false;
    }
    _lastError = "";
    return true;
}

bool gwStore_erase() {
    if (!_ready) return false;
    int rc = _store->remove(GW_KEY);
    return rc == MBED_SUCCESS || rc == MBED_ERROR_ITEM_NOT_FOUND;
}
