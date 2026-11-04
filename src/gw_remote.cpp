#include "gw_remote.h"

#include <stdio.h>
#include <string.h>

#include <Arduino_Portenta_OTA.h>
#include <BlockDevice.h>
#include <FATFileSystem.h>
#include <MBRBlockDevice.h>

#include "event_log.h"
#include "gw_config.h"
#include "gw_console.h"
#include "gw_status.h"
#include "ota_runner.h"

// ── Wire constants (see gw_remote.h for the full layout) ───────────────────
#define SUB_CONSOLE_EXEC  0x01
#define SUB_CONSOLE_READ  0x02
#define SUB_FW_BEGIN      0x10
#define SUB_FW_DATA       0x11
#define SUB_FW_STATUS     0x12
#define SUB_FW_APPLY      0x13
#define SUB_FW_ABORT      0x14

#define EXC_ILLEGAL_FUNCTION 0x01
#define EXC_ILLEGAL_VALUE    0x03
#define EXC_SERVER_FAILURE   0x04

#define CONSOLE_CHUNK_MAX  200      // data bytes per EXEC/READ response
#define CONSOLE_LINE_MAX   120      // mirrors GW_LINE_MAX in gw_console.cpp

// ── Console output capture ─────────────────────────────────────────────────
// One command's worth of console output, kept until the next EXEC so the
// client can page through it with CONSOLE_READ. 12 KB holds the biggest
// legitimate answer (LOG 100 ≈ 9 KB) with margin. Owned by whichever slot
// ran the EXEC: the other slot's READs are refused rather than fed lines
// from a conversation it never started.
static char    _out[12 * 1024];
static uint16_t _outLen = 0;
static int8_t  _ownerSlot = -1;
static bool    _overflow = false;

static void sinkAppend(const char* line) {
    if (_overflow) return;
    const size_t n = strlen(line);
    if (_outLen + n + 2 > sizeof(_out)) {
        // Stop appending entirely: the terminal #OK/#ERR line then never
        // arrives, which is how the client learns the answer was cut.
        _overflow = true;
        return;
    }
    memcpy(_out + _outLen, line, n);
    _outLen += (uint16_t)n;
    _out[_outLen++] = '\r';
    _out[_outLen++] = '\n';
}

static int exception(uint8_t* out, uint8_t code) {
    out[0] = GW_REMOTE_FC | 0x80;
    out[1] = code;
    return 2;
}

// resp: fc · sub · total u16 · off u16 · len u8 · data[len]
static int consoleChunk(uint8_t* out, uint8_t sub, uint16_t off) {
    uint16_t len = 0;
    if (off < _outLen) {
        len = _outLen - off;
        if (len > CONSOLE_CHUNK_MAX) len = CONSOLE_CHUNK_MAX;
    }
    out[0] = GW_REMOTE_FC;
    out[1] = sub;
    out[2] = (uint8_t)(_outLen >> 8);
    out[3] = (uint8_t)(_outLen & 0xFF);
    out[4] = (uint8_t)(off >> 8);
    out[5] = (uint8_t)(off & 0xFF);
    out[6] = (uint8_t)len;
    if (len) memcpy(out + 7, _out + off, len);
    return 7 + (int)len;
}

static int doConsoleExec(const uint8_t* body, int bodyLen, uint8_t* out, int slot) {
    if (bodyLen < 1 || bodyLen > CONSOLE_LINE_MAX)
        return exception(out, EXC_ILLEGAL_VALUE);
    char line[CONSOLE_LINE_MAX + 1];
    for (int i = 0; i < bodyLen; i++) {
        const uint8_t b = body[i];
        if (b < 0x20 || b > 0x7E)            // the USB parser's same rule
            return exception(out, EXC_ILLEGAL_VALUE);
        line[i] = (char)b;
    }
    line[bodyLen] = '\0';

    _outLen = 0;
    _overflow = false;
    _ownerSlot = (int8_t)slot;
    gwConsole_execute(line, sinkAppend);

    return consoleChunk(out, SUB_CONSOLE_EXEC, 0);
}

static int doConsoleRead(const uint8_t* body, int bodyLen, uint8_t* out, int slot) {
    if (bodyLen != 2) return exception(out, EXC_ILLEGAL_VALUE);
    if (_ownerSlot != (int8_t)slot) return exception(out, EXC_SERVER_FAILURE);
    const uint16_t off = ((uint16_t)body[0] << 8) | body[1];
    return consoleChunk(out, SUB_CONSOLE_READ, off);
}

// ── Firmware staging (subs 0x10-0x14) ──────────────────────────────────────
// The image streams into UPDATE.BIN on the QSPI OTA partition (the 5 MB FAT
// at partition 2 that Arduino's QSPIFormat lays down and nothing else uses).
// Applying is the Arduino factory bootloader's own OTA mechanism: RTC backup
// DR0=0x07AA + DR1..DR3 describe the staged file, and the NEXT boot copies it
// into internal flash. Arduino_Portenta_OTA supplies exactly two things — the
// bootloader capability probe and that register write (bootloader ABI we must
// not guess at). Its begin() is deliberately NOT used: begin() also mounts
// the WiFi partition to check a TLS certificate, which only its cloud
// download path needs, and this gateway may not have one.
//
// THE invariant: those RTC registers are written only in FW_APPLY, only after
// a full CRC pass over the bytes read back from flash. Power cut, dropped
// socket or expired session at any other moment leaves a file nothing will
// ever apply.

#define FW_UPDATE_PATH   "/fs/UPDATE.BIN"   // "fs": the library's open() looks
                                            // for this exact mount name
#define FW_PARTITION     2
#define FW_MAX_SIZE      (768UL * 1024UL)   // the whole app region
#define FW_MIN_SIZE      (50UL * 1024UL)    // smaller is no gateway image
#define FW_IDLE_ABORT_MS 60000UL
#define FW_RESET_DELAY_MS 300UL

enum FwState : uint8_t { FW_IDLE = 0, FW_RECEIVING = 1, FW_STAGED = 2, FW_ERROR = 3 };

// Status codes shared with the tool (see gw_remote.h).
#define FWS_OK           0
#define FWS_LOCKED       1
#define FWS_STORAGE      2
#define FWS_TOO_BIG      3
#define FWS_SEQ_MISMATCH 5
#define FWS_NOT_STARTED  6
#define FWS_CRC_MISMATCH 7
#define FWS_INCOMPLETE   8
#define FWS_NOT_CAPABLE  9
#define FWS_LIB_ERROR    10
#define FWS_FULL         11
#define FWS_BAD_NAME     12
#define FWS_BUSY         13
#define FWS_NOT_FOUND    14

static FwState  _fwState = FW_IDLE;
static uint32_t _fwSize, _fwCrc, _fwReceived;
static mbed::MBRBlockDevice* _fwPart = nullptr;
static mbed::FATFileSystem*  _fwFs = nullptr;
static FILE*    _fwFile = nullptr;
static unsigned long _fwLastMs = 0;
static unsigned long _resetAt = 0;

// CRC-32/ISO-HDLC (zlib's), bitwise — runs at ~1 ms/KB, and APPLY kicks the
// watchdog every 4 KB block anyway.
static uint32_t crc32_step(uint32_t crc, const uint8_t* p, size_t n) {
    crc = ~crc;
    while (n--) {
        crc ^= *p++;
        for (int k = 0; k < 8; k++)
            crc = (crc >> 1) ^ (0xEDB88320UL & (0UL - (crc & 1)));
    }
    return ~crc;
}

static void fwCloseFile() {
    if (_fwFile) { fclose(_fwFile); _fwFile = nullptr; }
}

static void dpClose();                  // the depot below shares this mount

static void fwUnmount() {
    dpClose();
    fwCloseFile();
    if (_fwFs)   { _fwFs->unmount(); delete _fwFs; _fwFs = nullptr; }
    if (_fwPart) { delete _fwPart; _fwPart = nullptr; }
}

static void fwReset(FwState to) {
    fwUnmount();
    _fwState = to;
    _fwReceived = 0;
}

// Mount the OTA partition as "fs". The partition holds nothing but our
// UPDATE.BIN, so a mount broken by a power cut mid-write is repaired by
// formatting it — self-healing beats a bricked staging area.
static bool fwMount() {
    mbed::BlockDevice* root = mbed::BlockDevice::get_default_instance();
    if (!root || root->init() != 0) return false;
    _fwPart = new mbed::MBRBlockDevice(root, FW_PARTITION);
    _fwFs = new mbed::FATFileSystem("fs");
    if (_fwFs->mount(_fwPart) != 0) {
        gwStatus_watchdogKick();            // a 5 MB format takes a moment
        if (mbed::FATFileSystem::format(_fwPart) != 0 ||
            _fwFs->mount(_fwPart) != 0) {
            fwUnmount();
            return false;
        }
    }
    return true;
}

static uint32_t rdU32(const uint8_t* p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | p[3];
}

static int fwStatusResp(uint8_t* out, uint8_t sub, uint8_t status) {
    out[0] = GW_REMOTE_FC;
    out[1] = sub;
    out[2] = status;
    return 3;
}

static int doFwBegin(const uint8_t* body, int bodyLen, uint8_t* out) {
    if (bodyLen != 8) return exception(out, EXC_ILLEGAL_VALUE);
    if (!gwConsole_armed())
        return fwStatusResp(out, SUB_FW_BEGIN, FWS_LOCKED);
    if (otaRunner_running())
        return fwStatusResp(out, SUB_FW_BEGIN, FWS_BUSY);
    const uint32_t size = rdU32(body);
    const uint32_t crc  = rdU32(body + 4);
    if (size < FW_MIN_SIZE || size > FW_MAX_SIZE)
        return fwStatusResp(out, SUB_FW_BEGIN, FWS_TOO_BIG);

    // A BEGIN over a previous attempt (either slot) starts fresh: one
    // operator, and the armed session is the gate that matters.
    fwReset(FW_IDLE);
    if (!fwMount())
        return fwStatusResp(out, SUB_FW_BEGIN, FWS_STORAGE);
    remove(FW_UPDATE_PATH);
    _fwFile = fopen(FW_UPDATE_PATH, "wb");
    if (!_fwFile) {
        fwUnmount();
        return fwStatusResp(out, SUB_FW_BEGIN, FWS_STORAGE);
    }
    _fwSize = size;
    _fwCrc = crc;
    _fwReceived = 0;
    _fwState = FW_RECEIVING;
    _fwLastMs = millis();
    gwConsole_refreshSession();
    eventLog_note(GW_EV_FW, 1, (uint16_t)(size >> 10));
    return fwStatusResp(out, SUB_FW_BEGIN, FWS_OK);
}

static int doFwData(const uint8_t* body, int bodyLen, uint8_t* out) {
    // resp: fc · sub · status u8 · received u32
    if (bodyLen < 5) return exception(out, EXC_ILLEGAL_VALUE);
    const uint32_t offset = rdU32(body);
    const uint8_t* data = body + 4;
    const int dataLen = bodyLen - 4;

    uint8_t status = FWS_OK;
    if (_fwState != FW_RECEIVING || !_fwFile) {
        status = FWS_NOT_STARTED;
    } else if (offset != _fwReceived || _fwReceived + dataLen > _fwSize) {
        status = FWS_SEQ_MISMATCH;          // received tells the tool where
    } else if (fwrite(data, 1, dataLen, _fwFile) != (size_t)dataLen) {
        fwReset(FW_ERROR);
        status = FWS_STORAGE;
    } else {
        _fwReceived += dataLen;
        _fwLastMs = millis();
        gwStatus_watchdogKick();
        gwConsole_refreshSession();         // a long upload keeps its session
        if (_fwReceived == _fwSize) {
            fwCloseFile();                  // FS stays mounted for APPLY
            _fwState = FW_STAGED;
        }
    }
    out[0] = GW_REMOTE_FC;
    out[1] = SUB_FW_DATA;
    out[2] = status;
    out[3] = (uint8_t)(_fwReceived >> 24);
    out[4] = (uint8_t)(_fwReceived >> 16);
    out[5] = (uint8_t)(_fwReceived >> 8);
    out[6] = (uint8_t)_fwReceived;
    return 7;
}

static int doFwStatus(uint8_t* out) {
    // resp: fc · sub · state u8 · received u32 · size u32 · capable u8 · boot_ver u8
    const uint8_t* boot = (const uint8_t*)(0x08000000UL + 0x1F000UL);
    out[0] = GW_REMOTE_FC;
    out[1] = SUB_FW_STATUS;
    out[2] = (uint8_t)_fwState;
    out[3] = (uint8_t)(_fwReceived >> 24);
    out[4] = (uint8_t)(_fwReceived >> 16);
    out[5] = (uint8_t)(_fwReceived >> 8);
    out[6] = (uint8_t)_fwReceived;
    out[7] = (uint8_t)(_fwSize >> 24);
    out[8] = (uint8_t)(_fwSize >> 16);
    out[9] = (uint8_t)(_fwSize >> 8);
    out[10] = (uint8_t)_fwSize;
    out[11] = Arduino_Portenta_OTA::isOtaCapable() ? 1 : 0;
    out[12] = boot[1];                      // bootloader version byte
    return 13;
}

static int doFwApply(uint8_t* out) {
    if (!gwConsole_armed())
        return fwStatusResp(out, SUB_FW_APPLY, FWS_LOCKED);
    if (_fwState != FW_STAGED)
        return fwStatusResp(out, SUB_FW_APPLY,
                            _fwState == FW_RECEIVING ? FWS_INCOMPLETE
                                                     : FWS_NOT_STARTED);
    if (!Arduino_Portenta_OTA::isOtaCapable())
        return fwStatusResp(out, SUB_FW_APPLY, FWS_NOT_CAPABLE);

    // Verify what is actually on the flash, not what we think we wrote.
    FILE* f = fopen(FW_UPDATE_PATH, "rb");
    if (!f) return fwStatusResp(out, SUB_FW_APPLY, FWS_STORAGE);
    static uint8_t block[4096];
    uint32_t crc = 0, total = 0;
    size_t n;
    while ((n = fread(block, 1, sizeof(block), f)) > 0) {
        crc = crc32_step(crc, block, n);
        total += n;
        gwStatus_watchdogKick();
    }
    fclose(f);
    if (total != _fwSize || crc != _fwCrc) {
        eventLog_note(GW_EV_FW, 3, (uint16_t)(total >> 10));
        fwReset(FW_ERROR);
        return fwStatusResp(out, SUB_FW_APPLY, FWS_CRC_MISMATCH);
    }

    // The library's update() = open() (find /fs/UPDATE.BIN, take its length)
    // + write() (the four RTC backup registers). Our mount IS "/fs".
    Arduino_Portenta_OTA_QSPI ota(QSPI_FLASH_FATFS_MBR, FW_PARTITION);
    if (ota.update() != Arduino_Portenta_OTA::Error::None)
        return fwStatusResp(out, SUB_FW_APPLY, FWS_LIB_ERROR);

    eventLog_note(GW_EV_FW, 2, (uint16_t)(_fwSize >> 10));
    _resetAt = millis() + FW_RESET_DELAY_MS;
    return fwStatusResp(out, SUB_FW_APPLY, FWS_OK);
}

static int doFwAbort(uint8_t* out) {
    fwReset(FW_IDLE);
    _fwSize = _fwCrc = 0;
    return fwStatusResp(out, SUB_FW_ABORT, FWS_OK);
}

// ── Depot (subs 0x20-0x24) ─────────────────────────────────────────────────
// Module firmware images kept on the same OTA FAT partition as UPDATE.BIN, so a laptop plugged straight into this Opta
// can leave one behind and the Tool on the hospital server can read it back later and run the module OTA itself
// (docs/design-module-depot.md). The gateway never executes or interprets these bytes: it stores, lists, returns and
// deletes them. Four slots, one file each, /fs/MODDEPn.BIN = a 64-byte header, then the image:
//   "LGSD" · flag u8 (0 = being written, 1 = complete and CRC-checked) · name_len u8 · name[24] · size u32 · crc32 u32
// The flag is set only after a CRC pass over the bytes read back from flash, so a power cut or a dropped socket leaves
// a slot that lists as incomplete and is never offered for reading.

#define SUB_DEPOT_BEGIN   0x20
#define SUB_DEPOT_DATA    0x21
#define SUB_DEPOT_LIST    0x22
#define SUB_DEPOT_READ    0x23
#define SUB_DEPOT_DELETE  0x24

#define DP_SLOTS          4
#define DP_HDR            64
#define DP_NAME_MAX       24
#define DP_MIN_SIZE       1024UL
#define DP_MAX_SIZE       (128UL * 1024UL)      // the module's own slot is 61,440 B
#define DP_READ_MAX       200
#define DP_IDLE_ABORT_MS  60000UL
#define DP_MOUNT_IDLE_MS  10000UL


static bool          _fsHold = false;       // the OTA runner keeps the mount while it runs
static bool          _dpWriting = false;
static FILE*         _dpFile = nullptr;
static uint8_t       _dpSlot = 0;
static uint32_t      _dpSize = 0, _dpCrc = 0, _dpReceived = 0, _dpRun = 0;
static char          _dpName[24];
static uint8_t       _dpNameLen = 0;
static unsigned long _dpLastMs = 0;
static unsigned long _fsTouchMs = 0;            // last depot use of the shared mount

static void dpClose() {
    if (_dpFile) { fclose(_dpFile); _dpFile = nullptr; }
    _dpWriting = false;
    _dpReceived = 0;
}

static void dpPath(uint8_t slot, char* p) {
    snprintf(p, 24, "/fs/MODDEP%u.BIN", (unsigned)slot);
}

// The shared OTA-partition mount, taken on first use and let go again by gwRemote_update() when idle.
static bool dpMount() {
    if (!_fwFs && !fwMount()) return false;
    _fsTouchMs = millis();
    return true;
}

struct DpHdr {
    uint8_t  state;                  // 0 empty / unreadable, 1 incomplete, 2 complete
    uint8_t  nameLen;
    char     name[DP_NAME_MAX];
    uint32_t size, crc;
};

static void putU32(uint8_t* p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v;
}

static void dpReadHdr(uint8_t slot, DpHdr& h) {
    memset(&h, 0, sizeof(h));
    if (_dpWriting && slot == _dpSlot) {    // its header is still in the FILE buffer, not on the flash
        h.state = 1;
        h.nameLen = _dpNameLen;
        memcpy(h.name, _dpName, _dpNameLen);
        h.size = _dpSize;
        h.crc = _dpCrc;
        return;
    }
    char path[24];
    dpPath(slot, path);
    FILE* f = fopen(path, "rb");
    if (!f) return;
    uint8_t b[DP_HDR];
    const size_t n = fread(b, 1, DP_HDR, f);
    fclose(f);
    if (n != DP_HDR || memcmp(b, "LGSD", 4) != 0 || b[5] == 0 || b[5] > DP_NAME_MAX) return;
    h.nameLen = b[5];
    memcpy(h.name, b + 6, h.nameLen);
    h.size = rdU32(b + 30);
    h.crc  = rdU32(b + 34);
    h.state = b[4] == 1 ? 2 : 1;
}

static bool nameOk(const uint8_t* p, int n) {
    if (n < 1 || n > DP_NAME_MAX) return false;
    for (int i = 0; i < n; i++) {
        const uint8_t c = p[i];
        if (!((c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
              c == '.' || c == '_' || c == '-'))
            return false;
    }
    return true;
}

static int dpStatus(uint8_t* out, uint8_t sub, uint8_t status, uint8_t extra) {
    out[0] = GW_REMOTE_FC;
    out[1] = sub;
    out[2] = status;
    out[3] = extra;
    return 4;
}

bool gwRemote_fsMount() { return dpMount(); }
void gwRemote_fsHold(bool hold) { _fsHold = hold; if (hold) _fsTouchMs = millis(); }

int gwRemote_depotImage(uint8_t slot, uint8_t* buf, uint32_t cap, uint32_t& size, uint32_t& crc, char* name25, uint8_t& nameLen) {
    if (slot >= DP_SLOTS || !dpMount()) return 3;
    DpHdr h;
    dpReadHdr(slot, h);
    if (h.state != 2 || (_dpWriting && _dpSlot == slot)) return 1;
    if (h.size > cap) return 2;
    char path[24];
    dpPath(slot, path);
    FILE* f = fopen(path, "rb");
    if (!f) return 3;
    bool ok = fseek(f, DP_HDR, SEEK_SET) == 0;
    uint32_t got = 0;
    while (ok && got < h.size) {
        const size_t n = fread(buf + got, 1, (h.size - got) > 4096 ? 4096 : (h.size - got), f);
        if (n == 0) { ok = false; break; }
        got += (uint32_t)n;
        gwStatus_watchdogKick();
    }
    fclose(f);
    if (!ok || got != h.size) return 3;
    size = h.size; crc = h.crc; nameLen = h.nameLen;
    memcpy(name25, h.name, h.nameLen);
    name25[h.nameLen] = 0;
    return 0;
}

static int doDepotBegin(const uint8_t* body, int bodyLen, uint8_t* out) {
    // req: size u32 · crc32 u32 · name[1..24]      resp: status u8 · slot u8
    if (bodyLen < 9 || bodyLen > 8 + DP_NAME_MAX) return exception(out, EXC_ILLEGAL_VALUE);
    if (!nameOk(body + 8, bodyLen - 8)) return dpStatus(out, SUB_DEPOT_BEGIN, FWS_BAD_NAME, 0);
    if (!gwConsole_armed()) return dpStatus(out, SUB_DEPOT_BEGIN, FWS_LOCKED, 0);
    const uint32_t size = rdU32(body), crc = rdU32(body + 4);
    const int nameLen = bodyLen - 8;
    if (size < DP_MIN_SIZE || size > DP_MAX_SIZE) return dpStatus(out, SUB_DEPOT_BEGIN, FWS_TOO_BIG, 0);
    if (_fwState == FW_RECEIVING) return dpStatus(out, SUB_DEPOT_BEGIN, FWS_BUSY, 0);

    dpClose();
    if (!dpMount()) return dpStatus(out, SUB_DEPOT_BEGIN, FWS_STORAGE, 0);

    // The slot already holding this name is replaced; otherwise an empty one, then one whose upload never finished.
    // A complete image of another name is never taken.
    int slot = -1;
    for (int s = 0; s < DP_SLOTS && slot < 0; s++) {
        DpHdr h;
        dpReadHdr((uint8_t)s, h);
        if (h.state && h.nameLen == nameLen && memcmp(h.name, body + 8, nameLen) == 0) slot = s;
    }
    for (int want = 0; want <= 1 && slot < 0; want++) {
        for (int s = 0; s < DP_SLOTS && slot < 0; s++) {
            DpHdr h;
            dpReadHdr((uint8_t)s, h);
            if (h.state == want) slot = s;
        }
    }
    if (slot < 0) return dpStatus(out, SUB_DEPOT_BEGIN, FWS_FULL, 0);

    char path[24];
    dpPath((uint8_t)slot, path);
    _dpFile = fopen(path, "wb");
    if (!_dpFile) return dpStatus(out, SUB_DEPOT_BEGIN, FWS_STORAGE, 0);
    uint8_t hdr[DP_HDR];
    memset(hdr, 0, sizeof(hdr));
    memcpy(hdr, "LGSD", 4);
    hdr[4] = 0;
    hdr[5] = (uint8_t)nameLen;
    memcpy(hdr + 6, body + 8, nameLen);
    putU32(hdr + 30, size);
    putU32(hdr + 34, crc);
    if (fwrite(hdr, 1, DP_HDR, _dpFile) != DP_HDR) {
        dpClose();
        return dpStatus(out, SUB_DEPOT_BEGIN, FWS_STORAGE, 0);
    }
    _dpSlot = (uint8_t)slot;
    _dpSize = size; _dpCrc = crc; _dpReceived = 0; _dpRun = 0;
    _dpNameLen = (uint8_t)nameLen;
    memcpy(_dpName, body + 8, nameLen);
    _dpWriting = true;
    _dpLastMs = millis();
    gwConsole_refreshSession();
    return dpStatus(out, SUB_DEPOT_BEGIN, FWS_OK, (uint8_t)slot);
}

// Close the file, check what is really on the flash against the announced CRC, and only then mark the slot complete.
static uint8_t dpFinish() {
    char path[24];
    dpPath(_dpSlot, path);
    fclose(_dpFile);
    _dpFile = nullptr;
    _dpWriting = false;

    uint8_t status = FWS_OK;
    FILE* f = fopen(path, "rb");
    if (!f) return FWS_STORAGE;
    static uint8_t block[4096];
    if (fseek(f, DP_HDR, SEEK_SET) != 0) { fclose(f); return FWS_STORAGE; }
    uint32_t crc = 0, total = 0;
    size_t n;
    while ((n = fread(block, 1, sizeof(block), f)) > 0) {
        crc = crc32_step(crc, block, n);
        total += n;
        gwStatus_watchdogKick();
    }
    fclose(f);
    if (total != _dpSize || crc != _dpCrc || _dpRun != _dpCrc) {
        remove(path);                       // never leave a slot that looks like an image but is not one
        return FWS_CRC_MISMATCH;
    }
    f = fopen(path, "r+b");
    if (!f) return FWS_STORAGE;
    if (fseek(f, 4, SEEK_SET) != 0 || fputc(1, f) == EOF) status = FWS_STORAGE;
    fclose(f);
    if (status == FWS_OK) eventLog_note(GW_EV_FW, 4, (uint16_t)(_dpSize >> 10));
    return status;
}

static int doDepotData(const uint8_t* body, int bodyLen, uint8_t* out) {
    // req: offset u32 · data[1..240]      resp: status u8 · received u32
    if (bodyLen < 5) return exception(out, EXC_ILLEGAL_VALUE);
    const uint32_t offset = rdU32(body);
    const uint8_t* data = body + 4;
    const int dataLen = bodyLen - 4;

    uint8_t status = FWS_OK;
    if (!_dpWriting || !_dpFile) {
        status = FWS_NOT_STARTED;
    } else if (offset != _dpReceived || _dpReceived + dataLen > _dpSize) {
        status = FWS_SEQ_MISMATCH;          // received tells the tool where to resume
    } else if (fwrite(data, 1, dataLen, _dpFile) != (size_t)dataLen) {
        dpClose();
        status = FWS_STORAGE;
    } else {
        _dpRun = crc32_step(_dpRun, data, dataLen);
        _dpReceived += dataLen;
        _dpLastMs = millis();
        _fsTouchMs = _dpLastMs;
        gwStatus_watchdogKick();
        gwConsole_refreshSession();
        if (_dpReceived == _dpSize) {
            const uint32_t got = _dpReceived;
            status = dpFinish();
            _dpReceived = status == FWS_OK ? got : 0;
        }
    }
    out[0] = GW_REMOTE_FC;
    out[1] = SUB_DEPOT_DATA;
    out[2] = status;
    putU32(out + 3, _dpReceived);
    return 7;
}

static int doDepotList(const uint8_t* body, int bodyLen, uint8_t* out) {
    // req: slot u8      resp: slot u8 · state u8 (0 empty, 1 incomplete, 2 complete) · size u32 · crc32 u32 · name_len u8 · name
    if (bodyLen != 1 || body[0] >= DP_SLOTS) return exception(out, EXC_ILLEGAL_VALUE);
    if (!dpMount()) return exception(out, EXC_SERVER_FAILURE);
    DpHdr h;
    dpReadHdr(body[0], h);
    out[0] = GW_REMOTE_FC;
    out[1] = SUB_DEPOT_LIST;
    out[2] = body[0];
    out[3] = h.state;
    putU32(out + 4, h.size);
    putU32(out + 8, h.crc);
    out[12] = h.nameLen;
    memcpy(out + 13, h.name, h.nameLen);
    return 13 + h.nameLen;
}

static int doDepotRead(const uint8_t* body, int bodyLen, uint8_t* out) {
    // req: slot u8 · offset u32      resp: status u8 · len u8 · data[len <= 200]
    if (bodyLen != 5 || body[0] >= DP_SLOTS) return exception(out, EXC_ILLEGAL_VALUE);
    if (!dpMount()) return exception(out, EXC_SERVER_FAILURE);
    DpHdr h;
    dpReadHdr(body[0], h);
    out[0] = GW_REMOTE_FC;
    out[1] = SUB_DEPOT_READ;
    out[2] = FWS_OK;
    out[3] = 0;
    if (h.state != 2 || (_dpWriting && _dpSlot == body[0])) {
        out[2] = FWS_NOT_FOUND;
        return 4;
    }
    const uint32_t off = rdU32(body + 1);
    if (off >= h.size) return 4;
    uint32_t len = h.size - off;
    if (len > DP_READ_MAX) len = DP_READ_MAX;
    char path[24];
    dpPath(body[0], path);
    FILE* f = fopen(path, "rb");
    if (!f) { out[2] = FWS_STORAGE; return 4; }
    size_t n = 0;
    if (fseek(f, (long)(DP_HDR + off), SEEK_SET) == 0) n = fread(out + 4, 1, len, f);
    fclose(f);
    if (n != len) { out[2] = FWS_STORAGE; return 4; }
    out[3] = (uint8_t)len;
    return 4 + (int)len;
}

static int doDepotDelete(const uint8_t* body, int bodyLen, uint8_t* out) {
    // req: slot u8      resp: status u8 · slot u8
    if (bodyLen != 1 || body[0] >= DP_SLOTS) return exception(out, EXC_ILLEGAL_VALUE);
    if (!gwConsole_armed()) return dpStatus(out, SUB_DEPOT_DELETE, FWS_LOCKED, body[0]);
    if (!dpMount()) return dpStatus(out, SUB_DEPOT_DELETE, FWS_STORAGE, body[0]);
    if (_dpWriting && _dpSlot == body[0]) dpClose();
    DpHdr h;
    dpReadHdr(body[0], h);
    if (!h.state) return dpStatus(out, SUB_DEPOT_DELETE, FWS_NOT_FOUND, body[0]);
    char path[24];
    dpPath(body[0], path);
    const bool ok = remove(path) == 0;
    if (ok) eventLog_note(GW_EV_FW, 5, (uint16_t)(h.size >> 10));
    return dpStatus(out, SUB_DEPOT_DELETE, ok ? FWS_OK : FWS_STORAGE, body[0]);
}

static int doDepot(uint8_t sub, const uint8_t* body, int bodyLen, uint8_t* out) {
    switch (sub) {
        case SUB_DEPOT_BEGIN:  return doDepotBegin(body, bodyLen, out);
        case SUB_DEPOT_DATA:   return doDepotData(body, bodyLen, out);
        case SUB_DEPOT_LIST:   return doDepotList(body, bodyLen, out);
        case SUB_DEPOT_READ:   return doDepotRead(body, bodyLen, out);
        case SUB_DEPOT_DELETE: return doDepotDelete(body, bodyLen, out);
        default:               return exception(out, EXC_ILLEGAL_FUNCTION);
    }
}

static int doFw(uint8_t sub, const uint8_t* body, int bodyLen,
                uint8_t* out, int slot) {
    (void)slot;                 // FW state is global on purpose: one operator
    switch (sub) {
        case SUB_FW_BEGIN:  return doFwBegin(body, bodyLen, out);
        case SUB_FW_DATA:   return doFwData(body, bodyLen, out);
        case SUB_FW_STATUS: return doFwStatus(out);
        case SUB_FW_APPLY:  return doFwApply(out);
        case SUB_FW_ABORT:  return doFwAbort(out);
        default:            return exception(out, EXC_ILLEGAL_FUNCTION);
    }
}

// ── Entry ──────────────────────────────────────────────────────────────────
int gwRemote_handle(const uint8_t* pdu, int pdu_len,
                    uint8_t* out, int out_max, int slot) {
    (void)out_max;                      // every response fits 207 B by design
    if (!gwConfig_active().net_console || pdu_len < 2 || pdu[0] != GW_REMOTE_FC)
        return exception(out, EXC_ILLEGAL_FUNCTION);

    const uint8_t sub = pdu[1];
    const uint8_t* body = pdu + 2;
    const int bodyLen = pdu_len - 2;

    switch (sub) {
        case SUB_CONSOLE_EXEC: return doConsoleExec(body, bodyLen, out, slot);
        case SUB_CONSOLE_READ: return doConsoleRead(body, bodyLen, out, slot);
        case SUB_FW_BEGIN:
        case SUB_FW_DATA:
        case SUB_FW_STATUS:
        case SUB_FW_APPLY:
        case SUB_FW_ABORT:     return doFw(sub, body, bodyLen, out, slot);
        case SUB_DEPOT_BEGIN:
        case SUB_DEPOT_DATA:
        case SUB_DEPOT_LIST:
        case SUB_DEPOT_READ:
        case SUB_DEPOT_DELETE: return doDepot(sub, body, bodyLen, out);
        default:               return exception(out, EXC_ILLEGAL_FUNCTION);
    }
}

void gwRemote_update() {
    // The post-APPLY reset, deferred so the status-0 response reaches the
    // client first. The IWDG may keep running through a software reset, and
    // the bootloader's copy of a ~230 KB image can outlast an 8 s period —
    // so stretch the dog to its ceiling first: PR/RLR are writable while
    // running (only STOPPING the IWDG is impossible), and the next boot's
    // gwStatus_watchdogBegin() sets the configured period again.
    if (_resetAt != 0 && (long)(millis() - _resetAt) >= 0) {
        IWDG1->KR  = 0x5555;                // unlock PR/RLR
        IWDG1->PR  = 7;                     // LSI/256: ~32.7 s ceiling
        IWDG1->RLR = 0x0FFF;
        while (IWDG1->SR & (IWDG_SR_PVU | IWDG_SR_RVU)) {}
        IWDG1->KR  = 0xAAAA;                // reload with the new period
        NVIC_SystemReset();
    }

    // A receiving upload whose stream stopped: free the file and the mount.
    // The partial file is inert — nothing applies it — and the next
    // FW_BEGIN starts clean.
    if (_fwState == FW_RECEIVING &&
        (millis() - _fwLastMs) > FW_IDLE_ABORT_MS) {
        fwReset(FW_IDLE);
    }

    // A depot upload whose stream stopped: close the file. The slot stays flagged incomplete, so it is never offered for
    // reading, and the next DEPOT_BEGIN reuses it.
    if (_dpWriting && (millis() - _dpLastMs) > DP_IDLE_ABORT_MS) dpClose();

    // Let go of the shared mount once the depot has been idle for a while and no firmware stage needs it.
    if (_fwFs && _fwState == FW_IDLE && !_dpWriting && !_fsHold && (millis() - _fsTouchMs) > DP_MOUNT_IDLE_MS)
        fwUnmount();
}
