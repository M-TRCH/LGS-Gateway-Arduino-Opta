#include "wifi_ap.h"

#include <BlockDevice.h>
#include <FATFileSystem.h>
#include <MBRBlockDevice.h>
#include <malloc.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#include "config.h"
#include "event_log.h"
#include "gw_config.h"
#include "gw_remote.h"
#include "gw_status.h"
#include "gw_store.h"
#include "net_runtime.h"
#include "ota_runner.h"

// The radio half (WiFi.h, the WHD driver, the soft-AP) is compiled in only when this is 1, so the model and the
// firmware-file paths can be proven on a unit before the driver is linked at all.
#ifndef WIFIAP_RADIO
#define WIFIAP_RADIO 1
#endif

#if WIFIAP_RADIO
#include <WiFi.h>
#endif

// ── Radio firmware file on QSPI partition 1 ────────────────────────────────
// The driver mounts partition 1 itself as "wlan" when the radio starts; this code mounts it under a different name
// ("wfw") and always unmounts again, so the two never hold it at the same time.
#define WFW_PARTITION   1
#define WFW_DIR         "/wfw"
#define WFW_FILE        "/wfw/4343WA1.BIN"
#define WFW_TMP         "/wfw/4343WA1.TMP"
#define WFW_EXPECT      421098UL          // libmbed.a 4343WA1_bin.o, wifi_firmware_image_data
#define WFW_MIN         300000UL
#define WFW_MAX         600000UL
#define WFW_IDLE_MS     60000UL

#define FWS_OK           0
#define FWS_STORAGE      2
#define FWS_TOO_BIG      3
#define FWS_SEQ_MISMATCH 5
#define FWS_NOT_STARTED  6
#define FWS_CRC_MISMATCH 7
#define FWS_NOT_CAPABLE  9
#define FWS_BUSY         13

static const char* _fwState = nullptr;     // cached; nullptr = not checked yet

static mbed::MBRBlockDevice* _wPart = nullptr;
static mbed::FATFileSystem*  _wFs = nullptr;
static FILE*    _wFile = nullptr;
static bool     _wRecv = false;
static uint32_t _wSize = 0, _wCrc = 0, _wRun = 0, _wGot = 0;
static uint32_t _wLastMs = 0;

static bool wMount(bool formatIfNeeded) {
    if (_wFs) return true;
    mbed::BlockDevice* root = mbed::BlockDevice::get_default_instance();
    if (!root || root->init() != 0) return false;
    _wPart = new mbed::MBRBlockDevice(root, WFW_PARTITION);
    _wFs = new mbed::FATFileSystem("wfw");
    if (_wFs->mount(_wPart) == 0) return true;
    if (formatIfNeeded) {
        gwStatus_watchdogKick();
        // Partition 1 holds only the radio's files (QSPIFormat: "WiFi firmware and certificates 1MB").
        if (mbed::FATFileSystem::format(_wPart) == 0 && _wFs->mount(_wPart) == 0) return true;
    }
    delete _wFs; _wFs = nullptr;
    delete _wPart; _wPart = nullptr;
    return false;
}

static void wUnmount() {
    if (_wFile) { fclose(_wFile); _wFile = nullptr; }
    if (_wFs) { _wFs->unmount(); delete _wFs; _wFs = nullptr; }
    if (_wPart) { delete _wPart; _wPart = nullptr; }
}

const char* wifiAp_fwState() {
    if (!gwStatus_hasWifi()) return "n/a";
    if (_wRecv) return "installing";
    if (_fwState) return _fwState;
    if (!wMount(false)) return (_fwState = "nofs");
    struct stat st;
    if (stat(WFW_FILE, &st) != 0) _fwState = "missing";
    else if ((uint32_t)st.st_size != WFW_EXPECT) _fwState = "bad_size";
    else _fwState = "ok";
    wUnmount();
    return _fwState;
}

void wifiAp_fwInvalidate() { _fwState = nullptr; }

bool     wifiAp_fwReceiving() { return _wRecv; }
uint32_t wifiAp_fwReceived() { return _wGot; }

static void wAbort() {
    if (_wFile) { fclose(_wFile); _wFile = nullptr; }
    if (_wFs) remove(WFW_TMP);
    wUnmount();
    _wRecv = false;
    _wGot = 0;
}

uint8_t wifiAp_fwBegin(uint32_t size, uint32_t crc) {
    if (!gwStatus_hasWifi()) return FWS_NOT_CAPABLE;
    if (wifiAp_isUp() || otaRunner_running() || gwRemote_fwBusy()) return FWS_BUSY;
    if (size < WFW_MIN || size > WFW_MAX) return FWS_TOO_BIG;
    wAbort();
    if (!wMount(true)) return FWS_STORAGE;
    _wFile = fopen(WFW_TMP, "wb");
    if (!_wFile) { wUnmount(); return FWS_STORAGE; }
    _wSize = size; _wCrc = crc; _wRun = 0; _wGot = 0;
    _wRecv = true;
    _wLastMs = millis();
    _fwState = nullptr;
    return FWS_OK;
}

// The file as it is on the flash, read back, against the CRC announced at BEGIN; only then does it take the real name.
static uint8_t wFinish() {
    fclose(_wFile);
    _wFile = nullptr;
    _wRecv = false;
    FILE* f = fopen(WFW_TMP, "rb");
    if (!f) { wUnmount(); return FWS_STORAGE; }
    static uint8_t block[2048];
    uint32_t crc = 0, total = 0;
    size_t n;
    while ((n = fread(block, 1, sizeof(block), f)) > 0) {
        crc = gwRemote_crc32(crc, block, n);
        total += n;
        gwStatus_watchdogKick();
    }
    fclose(f);
    if (total != _wSize || crc != _wCrc || _wRun != _wCrc) {
        remove(WFW_TMP);
        wUnmount();
        eventLog_note(GW_EV_WIFI_FW, 2, (uint16_t)(total >> 10));
        return FWS_CRC_MISMATCH;
    }
    remove(WFW_FILE);
    const bool ok = rename(WFW_TMP, WFW_FILE) == 0;
    wUnmount();
    _fwState = nullptr;
    if (!ok) return FWS_STORAGE;
    eventLog_note(GW_EV_WIFI_FW, 1, (uint16_t)(total >> 10));
    return FWS_OK;
}

uint8_t wifiAp_fwData(uint32_t offset, const uint8_t* data, int len, uint32_t& received) {
    uint8_t st = FWS_OK;
    if (!_wRecv || !_wFile) st = FWS_NOT_STARTED;
    else if (offset != _wGot || _wGot + (uint32_t)len > _wSize) st = FWS_SEQ_MISMATCH;
    else if (fwrite(data, 1, len, _wFile) != (size_t)len) { wAbort(); st = FWS_STORAGE; }
    else {
        _wRun = gwRemote_crc32(_wRun, data, len);
        _wGot += len;
        _wLastMs = millis();
        gwStatus_watchdogKick();
        if (_wGot == _wSize) {
            const uint32_t got = _wGot;
            st = wFinish();
            _wGot = st == FWS_OK ? got : 0;
        }
    }
    received = _wGot;
    return st;
}

// ── The access point ───────────────────────────────────────────────────────
#define AP_IP_A 192
#define AP_IP_B 168
#define AP_IP_C 77
#define AP_CHANNEL        6
#define AP_DEF_MIN        30
#define AP_MAX_MIN        480
#define AP_PASS_KEY       "ap"
#define AP_PASS_MIN       8
#define AP_PASS_MAX       31

enum ApState : uint8_t { AP_OFF = 0, AP_STARTING = 1, AP_ON = 2, AP_FAILED = 3 };
static ApState  _ap = AP_OFF;
static bool     _wantOn = false, _wantOff = false;
static uint8_t  _who = 1;                 // 1 console, 2 panel
static bool     _bridge = false;
static uint16_t _idleMin = AP_DEF_MIN;
static uint32_t _onAt = 0, _lastUse = 0, _ledAt = 0;
static uint32_t _startMs = 0;              // how long the last start took
static uint16_t _starts = 0, _fails = 0;
static bool     _led = false;
static const char* _lastErr = "";

bool wifiAp_isUp() { return _ap == AP_ON; }
bool wifiAp_isApClient(const uint8_t ip[4]) { return ip[0] == AP_IP_A && ip[1] == AP_IP_B && ip[2] == AP_IP_C; }
bool wifiAp_bridgeAllowed() { return _bridge; }
void wifiAp_touch() { _lastUse = millis(); }

static bool loadPass(char* out, size_t cap) {
    size_t len = 0;
    if (!gwStore_auxGet(AP_PASS_KEY, out, cap - 1, &len) || len < AP_PASS_MIN || len > AP_PASS_MAX) return false;
    out[len] = 0;
    return true;
}

static const char* refuseReason() {
    if (!gwStatus_hasWifi()) return "no_radio";
    if (strcmp(wifiAp_fwState(), "ok") != 0) return "wifi_fw_not_installed";
    char p[AP_PASS_MAX + 2];
    if (!loadPass(p, sizeof(p))) return "no_password";
    if (_wRecv) return "wifi_fw_installing";
#if !WIFIAP_RADIO
    return "radio_not_built";
#endif
    return nullptr;
}

static void ledShow(bool on) { digitalWrite(LED_LINK_PIN, on ? HIGH : LOW); }

static void apStart() {
    const char* why = refuseReason();
    if (why) { _lastErr = why; _ap = AP_FAILED; eventLog_note(GW_EV_AP, 4, 0); return; }
#if WIFIAP_RADIO
    char pass[AP_PASS_MAX + 2];
    loadPass(pass, sizeof(pass));
    char ssid[40];
    snprintf(ssid, sizeof(ssid), "LGS-%s", gwConfig_active().sys_name[0] ? gwConfig_active().sys_name : "gateway");
    _ap = AP_STARTING;
    const uint32_t t0 = millis();
    // The driver loads the radio firmware from QSPI and brings the soft-AP and its DHCP server up: seconds, inside one
    // call. A hang in there must end in a reset, never a dead unit, and the AP is never started at boot, so it cannot loop.
    gwStatus_watchdogKick();
    gwStatus_watchdogStretch();
    WiFi.config("192.168.77.1", "255.255.255.0", "192.168.77.1");
    const int st = WiFi.beginAP(ssid, pass, AP_CHANNEL);
    gwStatus_watchdogRestore();
    gwStatus_watchdogKick();
    memset(pass, 0, sizeof(pass));
    _startMs = millis() - t0;
    _starts++;
    if (st == WL_AP_LISTENING || st == WL_AP_CONNECTED) {
        _ap = AP_ON;
        _onAt = _lastUse = millis();
        _lastErr = "";
        eventLog_note(GW_EV_AP, 1, _who);
    } else {
        _fails++;
        _ap = AP_FAILED;
        _lastErr = "begin_ap_failed";
        WiFi.end();
        eventLog_note(GW_EV_AP, 3, (uint16_t)st);
    }
#endif
}

static void apStop() {
#if WIFIAP_RADIO
    if (_ap == AP_ON) {
        gwStatus_watchdogKick();
        WiFi.disconnect();
        WiFi.end();
        gwStatus_watchdogKick();
        eventLog_note(GW_EV_AP, 2, (uint16_t)((millis() - _onAt) / 60000UL));
    }
#endif
    _ap = AP_OFF;
    _bridge = false;
    ledShow(netRuntime_isUp());
}

void wifiAp_update() {
    if (_wRecv && millis() - _wLastMs > WFW_IDLE_MS) wAbort();     // an install whose stream stopped

    if (_wantOff) { _wantOff = false; _wantOn = false; apStop(); return; }
    if (_wantOn) { _wantOn = false; if (_ap != AP_ON) apStart(); return; }
    if (_ap != AP_ON) return;

    const uint32_t now = millis();
#if WIFIAP_RADIO
    if (WiFi.status() == WL_AP_CONNECTED) _lastUse = now;          // somebody is associated: not idle
#endif
    if (now - _lastUse > (uint32_t)_idleMin * 60000UL) { apStop(); return; }
    if (now - _ledAt >= 500) { _ledAt = now; _led = !_led; ledShow(_led); }
}

void wifiAp_panelToggle() {
    _who = 2;
    if (_ap == AP_ON) _wantOff = true;
    else { _idleMin = AP_DEF_MIN; _bridge = false; _wantOn = true; }
}

// ── Console ────────────────────────────────────────────────────────────────
static char _ln[160];
static void out(void (*emit)(const char*), const char* fmt, ...) __attribute__((format(printf, 2, 3)));
static void out(void (*emit)(const char*), const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(_ln, sizeof(_ln), fmt, ap);
    va_end(ap);
    emit(_ln);
}

static const char* kv(char** argv, int argc, const char* key) {
    const size_t kl = strlen(key);
    for (int i = 0; i < argc; i++)
        if (!strncmp(argv[i], key, kl) && argv[i][kl] == '=') return argv[i] + kl + 1;
    return nullptr;
}

static const char* stateName() {
    switch (_ap) {
        case AP_STARTING: return "starting";
        case AP_ON:       return "on";
        case AP_FAILED:   return "failed";
        default:          return "off";
    }
}

void wifiAp_infoLine(char* o, size_t n) {
    const uint32_t left = (_ap == AP_ON) ? (uint32_t)_idleMin * 60UL - min((millis() - _lastUse) / 1000UL, (uint32_t)_idleMin * 60UL) : 0;
    struct mallinfo mi = mallinfo();
    snprintf(o, n, "hw.otp=%d hw.wifi=%d hw.rs485=%d hw.pid=0x%04X wifi.fw=%s ap.state=%s ap.left_s=%lu ap.bridge=%d ap.start_ms=%lu ap.heap_used=%lu",
             gwStatus_otpValid() ? 1 : 0, gwStatus_hasWifi() ? 1 : 0, (gwStatus_boardFunc() & 0x02) ? 1 : 0,
             (unsigned)gwStatus_boardPid(), wifiAp_fwState(), stateName(), (unsigned long)left, _bridge ? 1 : 0,
             (unsigned long)_startMs, (unsigned long)mi.uordblks);
}

void wifiAp_command(char** argv, int argc, bool armed, void (*emit)(const char*)) {
    const char* sub = argc > 0 ? argv[0] : "STATUS";
    if (!strcasecmp(sub, "STATUS")) {
        out(emit, "#DATA ap.state=%s ap.err=%s ap.starts=%u ap.fails=%u ap.idle_min=%u ap.bridge=%d ap.start_ms=%lu wifi.fw=%s",
            stateName(), _lastErr[0] ? _lastErr : "-", _starts, _fails, _idleMin, _bridge ? 1 : 0, (unsigned long)_startMs,
            wifiAp_fwState());
#if WIFIAP_RADIO
        if (_ap == AP_ON) out(emit, "#DATA ap.ssid=%s ap.ip=192.168.77.1 ap.client=%d", WiFi.SSID(), WiFi.status() == WL_AP_CONNECTED ? 1 : 0);
#endif
        emit("#OK AP");
        return;
    }
    if (!armed) { emit("#ERR AP err=locked hint=HELLO"); return; }
    if (!strcasecmp(sub, "ON")) {
        const char* v = kv(argv + 1, argc - 1, "min");
        const long m = v ? strtol(v, nullptr, 10) : AP_DEF_MIN;
        if (m < 1 || m > AP_MAX_MIN) { emit("#ERR AP err=range key=min allowed=1-480"); return; }
        v = kv(argv + 1, argc - 1, "bridge");
        const char* why = refuseReason();
        if (why) { out(emit, "#ERR AP err=refused detail=%s", why); return; }
        _idleMin = (uint16_t)m;
        _bridge = v && !strcmp(v, "1");
        _who = 1;
        _wantOn = true;
        out(emit, "#OK AP ON min=%ld bridge=%d note=starting_from_loop", m, _bridge ? 1 : 0);
        return;
    }
    if (!strcasecmp(sub, "OFF")) {
        _wantOff = true;
        emit("#OK AP OFF");
        return;
    }
    if (!strcasecmp(sub, "PASS")) {
        if (argc < 2) { emit("#ERR AP err=syntax want=PASS_<8-31_chars>"); return; }
        const char* p = argv[1];
        const size_t n = strlen(p);
        if (n < AP_PASS_MIN || n > AP_PASS_MAX) { emit("#ERR AP err=range key=pass allowed=8-31_chars"); return; }
        for (size_t i = 0; i < n; i++) if (p[i] < 0x21 || p[i] > 0x7E) { emit("#ERR AP err=range key=pass allowed=printable"); return; }
        if (!gwStore_auxSet(AP_PASS_KEY, p, n)) { emit("#ERR AP err=storage"); return; }
        emit("#OK AP PASS stored=1");          // never echoed back
        return;
    }
    emit("#ERR AP err=syntax want=STATUS|ON|OFF|PASS");
}
