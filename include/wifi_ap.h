#ifndef WIFI_AP_H
#define WIFI_AP_H

#include <Arduino.h>

// ── Service Wi-Fi access point (Opta WiFi only) ────────────────────────────
// docs/design-service-ap.md. Off by default, never started at boot, started and stopped from loop() on a console verb
// or the panel action, off again by itself. The radio's firmware file lives on QSPI partition 1 (`/wlan/4343WA1.BIN`);
// without it the core's driver spins forever, so nothing here touches the driver until that file has been checked.

// The radio firmware file on partition 1: "ok", "missing", "bad_size", "nofs" (partition 1 does not mount) or "n/a"
// (not an Opta WiFi). Checked once, then cached until an install changes it.
const char* wifiAp_fwState();
void        wifiAp_fwInvalidate();

// Network install of the radio firmware (gw_remote subfunctions 0x30-0x32). Returns a FWS_* style status byte.
uint8_t  wifiAp_fwBegin(uint32_t size, uint32_t crc);
uint8_t  wifiAp_fwData(uint32_t offset, const uint8_t* data, int len, uint32_t& received);
uint32_t wifiAp_fwReceived();
bool     wifiAp_fwReceiving();

// AP lifecycle.
void wifiAp_update();                 // every loop()
bool wifiAp_isUp();                   // the AP is on (NTP pauses, the bridge filters AP clients)
bool wifiAp_isApClient(const uint8_t ip[4]);  // a client address inside the AP subnet
bool wifiAp_bridgeAllowed();          // AP clients may use the Modbus bridge (AP ON bridge=1)
bool wifiAp_httpAllowed(const uint8_t ip[4]);  // the page is served to this client (AP subnet; LAN only with AP TESTLAN 1)
void wifiAp_touch();                  // an AP client did something: restart the idle timer
void wifiAp_panelToggle();            // the front-panel action

// Console verb `AP ...` and the INFO line (no spaces inside values).
void wifiAp_command(char** argv, int argc, bool armed, void (*emit)(const char* line));
void wifiAp_info(void (*emit)(const char* line));        // two #DATA lines for INFO

#endif // WIFI_AP_H
