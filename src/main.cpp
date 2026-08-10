#include <ArduinoRS485.h>
#include <mbed_wait_api.h>
#include <drivers/Watchdog.h>

#include "config.h"
#include "gw_config.h"
#include "gw_console.h"
#include "gw_status.h"
#include "gw_store.h"
#include "net_runtime.h"
#include "tcp_bridge.h"
#include "panel.h"
#include "usb_bridge.h"
#include "version.h"

#define WATCHDOG_MS       8000      // >> the worst legitimate stall (~2.2 s)
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

    pinMode(MODULE_RELAY_PIN, OUTPUT); digitalWrite(MODULE_RELAY_PIN, HIGH);
    pinMode(LED_RELAY_PIN,    OUTPUT); digitalWrite(LED_RELAY_PIN,    HIGH);
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

    // From here the bridge is live. Everything above is bounded; everything
    // below is optional. This ordering is the whole safety story: no stored
    // value and no storage failure can cost the USB bridge.
    panel_begin();
    usbBridge_begin();
    gwConsole_begin();
    digitalWrite(USB_MODE_LED_PIN, HIGH);

    // Optional, and deliberately last: the worst a missing cable or a wrong
    // address can cost is net.link_timeout_ms of extra boot time.
    netRuntime_begin();

    LOG_SERIAL.print("[SYS] LGS gateway "); LOG_SERIAL.print(GW_FW_VERSION);
    LOG_SERIAL.print(" up, config="); LOG_SERIAL.print(gwConfig_sourceName());
    LOG_SERIAL.print(", net="); LOG_SERIAL.println(netRuntime_stateName());

    mbed::Watchdog::get_instance().start(WATCHDOG_MS);
}

void loop() {
    mbed::Watchdog::get_instance().kick();

    usbBridge_update();
    gwConsole_update();
    netRuntime_update();
    if (netRuntime_isUp()) tcpBridge_update();
    gwStatus_update();
    panel_update();

    if (!_healthyMarked && millis() > HEALTHY_AFTER_MS) {
        gwStatus_markHealthy();
        _healthyMarked = true;
    }
}
