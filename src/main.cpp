#include <ArduinoRS485.h>
#include <mbed_wait_api.h>

#include "config.h"
#include "event_log.h"
#include "gw_config.h"
#include "gw_console.h"
#include "gw_remote.h"
#include "gw_status.h"
#include "gw_store.h"
#include "net_runtime.h"
#include "ntp.h"
#include "tcp_bridge.h"
#include "panel.h"
#include "sched.h"
#include "usb_bridge.h"
#include "version.h"

#define HEALTHY_AFTER_MS  10000     // loop must run this long to clear the
                                    // boot-attempt counter
#define BTN_RECOVERY_MS   3000
#define BTN_ERASE_MS      10000

static bool _healthyMarked = false;

// Hold the on-board button through boot to get back to a known state without
// any tooling. Nothing here ever writes to the store except the explicit
// 10 s erase.
static void sampleBootButton(bool& forceDefaults, bool& eraseStore) {
    const int idle = gwStatus_buttonRaw();
    unsigned long t0 = millis();
    unsigned long held = 0;

    while (millis() - t0 < BTN_ERASE_MS + 500UL) {
        if (gwStatus_buttonRaw() == idle) break;     // released
        held = millis() - t0;
        // Blink the fault LED so the operator can count the hold.
        digitalWrite(LED_FAULT_PIN, ((held / 250) % 2) ? HIGH : LOW);
        delay(10);
    }
    digitalWrite(LED_FAULT_PIN, LOW);

    forceDefaults = held >= BTN_RECOVERY_MS;
    eraseStore    = held >= BTN_ERASE_MS;
}

void setup() {
    Serial.begin(SERIAL_BAUD);

    // The shelf's power comes up here, before anything can fail, and stays
    // up: panel_begin() takes the outputs over once the config is loaded and
    // energises whichever one is mapped to the shelf. Doing it in this order
    // means a store that will not load costs the cabinet nothing.
    pinMode(PANEL_OUT_1, OUTPUT); digitalWrite(PANEL_OUT_1, HIGH);
    pinMode(USB_MODE_LED_PIN, OUTPUT); digitalWrite(USB_MODE_LED_PIN, LOW);

    // Latches the reset reason, bumps the boot-attempt counter and caches the
    // OTP MAC — the MAC must be read before any QSPI block device is mounted.
    gwStatus_begin();

    bool forceDefaults = false, eraseStore = false;
    sampleBootButton(forceDefaults, eraseStore);

    // Three failed boots in a row also force defaults, so a stored value that
    // hangs the board heals itself without tooling.
    if (gwStatus_safeMode()) forceDefaults = true;

    gwStore_begin();
    if (eraseStore) gwStore_erase();
    gwConfig_begin(forceDefaults);

    // The earliest point the configured period is known. Everything below can
    // block; nothing below is allowed to hang the cabinet.
    gwStatus_watchdogBegin(gwConfig_active().sys_wdt_ms);

    // The log's 1.5 MB boot scan sits under the watchdog just started; the
    // QSPI itself was mounted by gwStore_begin() and the OTP MAC read long
    // before that. Writes its own boot event (with the reset cause) — the
    // store operations above happened before the log existed, so they are
    // reported here instead.
    eventLog_begin();
    if (eraseStore)         eventLog_note(GW_EV_STORE_ERASED, 1, 0);
    else if (forceDefaults) eventLog_note(GW_EV_STORE_ERASED, 2, 0);

    // From here the bridge is live. Everything above is bounded; everything
    // below is optional. This ordering is the whole safety story: no stored
    // value and no storage failure can cost the USB bridge.
    panel_begin();
    sched_begin();
    usbBridge_begin();
    gwConsole_begin();
    digitalWrite(USB_MODE_LED_PIN, HIGH);

    // Optional, and deliberately last: the worst a missing cable or a wrong
    // address can cost is net.link_timeout_ms of extra boot time. That wait is
    // the longest legitimate stall in the whole boot, so the watchdog is fed
    // immediately before it — and gwConfig_validateStaged() refuses a period
    // too short to survive it.
    gwStatus_watchdogKick();
    netRuntime_begin();

    LOG_SERIAL.print("[SYS] LGS gateway "); LOG_SERIAL.print(GW_FW_VERSION);
    LOG_SERIAL.print(" up, config="); LOG_SERIAL.print(gwConfig_sourceName());
    LOG_SERIAL.print(", net="); LOG_SERIAL.print(netRuntime_stateName());
    LOG_SERIAL.print(", wdt="); LOG_SERIAL.println(gwStatus_watchdogMs());
}

void loop() {
    gwStatus_watchdogKick();

    usbBridge_update();
    gwConsole_update();
    gwRemote_update();          // post-APPLY reset + stalled-upload timeout
    netRuntime_update();
    if (netRuntime_isUp()) tcpBridge_update();
    ntp_update();               // checks enabled/link state itself
    gwStatus_update();
    panel_update();
    sched_update();

    if (!_healthyMarked && millis() > HEALTHY_AFTER_MS) {
        gwStatus_markHealthy();
        _healthyMarked = true;
    }
}
