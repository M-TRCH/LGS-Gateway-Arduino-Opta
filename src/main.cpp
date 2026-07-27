#include <Ethernet.h>
#include <ArduinoRS485.h>
#include "config.h"
#include "tcp_bridge.h"
#include "self_test.h"
#include "usb_bridge.h"

// ── Network identity (values in config.h) ──────────────────────────────────
static byte      MAC_ADDR[] = NET_MAC_ADDRESS;
static IPAddress STATIC_IP(NET_STATIC_IP);

// ── Operating mode ─────────────────────────────────────────────────────────
// Fixed at boot by USB_BRIDGE_ON_BOOT while the panel buttons are disabled;
// the Green button toggles it at runtime when PANEL_BUTTONS_ENABLED is 1.
static bool usbBridgeMode = (USB_BRIDGE_ON_BOOT != 0);

#if PANEL_BUTTONS_ENABLED

// ── Hardware reset ─────────────────────────────────────────────────────────
static void hardwareReset() {
    if (!usbBridgeMode) LOG_SERIAL.println("[SYS] Hardware reset...");
    digitalWrite(MODULE_RELAY_PIN, LOW);
    digitalWrite(LED_RELAY_PIN,    LOW);
    delay(RESET_RELAY_SETTLE_MS);
    NVIC_SystemReset();
}

// ── Buttons ────────────────────────────────────────────────────────────────
// Pressed = HIGH that persists across the debounce gap.
static bool buttonPressed(pin_size_t pin) {
    if (digitalRead(pin) != HIGH) return false;
    delay(BTN_DEBOUNCE_MS);
    return digitalRead(pin) == HIGH;
}

// ── Mode switching (Green button) ──────────────────────────────────────────
static void enterUsbBridgeMode() {
    LOG_SERIAL.println("[MODE] USB-RS485 bridge ON — serial logging suspended.");
    tcpBridge_dropClient();
    usbBridge_begin();
    digitalWrite(USB_MODE_LED_PIN, HIGH);
}

static void leaveUsbBridgeMode() {
    usbBridge_end();
    digitalWrite(USB_MODE_LED_PIN, LOW);
    LOG_SERIAL.println("[MODE] USB-RS485 bridge OFF — TCP gateway resumed.");
}

#endif // PANEL_BUTTONS_ENABLED

// ── Setup ──────────────────────────────────────────────────────────────────
void setup() {
    Serial.begin(SERIAL_BAUD);
    delay(500);
    LOG_SERIAL.println("========================================");
    LOG_SERIAL.println("  Modbus TCP-RTU Gateway");
    LOG_SERIAL.println("  Board: Arduino Opta");
    LOG_SERIAL.println("========================================");

    pinMode(MODULE_RELAY_PIN, OUTPUT); digitalWrite(MODULE_RELAY_PIN, HIGH);
    pinMode(LED_RELAY_PIN,    OUTPUT); digitalWrite(LED_RELAY_PIN,    HIGH);
#if PANEL_BUTTONS_ENABLED
    pinMode(SW_R_PIN, INPUT);
    pinMode(SW_G_PIN, INPUT);
    pinMode(SW_B_PIN, INPUT);
    pinMode(SW_Y_PIN, INPUT);
    pinMode(SW_W_PIN, INPUT);
#endif
    pinMode(USB_MODE_LED_PIN, OUTPUT); digitalWrite(USB_MODE_LED_PIN, LOW);

    LOG_SERIAL.println("[INIT] Configuring RS485...");
    RS485.setDelays(RS485_PRE_DELAY_US, RS485_POST_DELAY_US);
    RS485.begin(RS485_BAUD);
    RS485.receive();
    LOG_SERIAL.print("[INIT] RS485 ready @ "); LOG_SERIAL.print(RS485_BAUD); LOG_SERIAL.println(" baud");

    tcpBridge_init(MAC_ADDR, STATIC_IP);

    if (usbBridgeMode) {
        // Mode fixed in code: pure USB-RS485 converter from boot.
        // No startup coil sweep — the RS485 bus belongs to the PC master.
        LOG_SERIAL.println("[MODE] USB-RS485 bridge (fixed in code) — serial logging suspended.");
        LOG_SERIAL.println("========================================");
        usbBridge_begin();
        digitalWrite(USB_MODE_LED_PIN, HIGH);
        return;
    }

    LOG_SERIAL.println("[INIT] Gateway ONLINE — waiting for connections...");
    LOG_SERIAL.println("========================================");

    delay(STARTUP_SWEEP_DELAY_MS);
    selfTest_coilSweep();
}

// ── Loop ───────────────────────────────────────────────────────────────────
void loop() {
#if PANEL_BUTTONS_ENABLED
    // Green button toggles between TCP-gateway and USB-RS485 bridge mode.
    if (buttonPressed(SW_G_PIN)) {
        usbBridgeMode = !usbBridgeMode;
        if (usbBridgeMode) enterUsbBridgeMode();
        else               leaveUsbBridgeMode();
        while (digitalRead(SW_G_PIN) == HIGH) delay(10);   // wait for release
    }

    // White button → hardware reset (available in both modes).
    if (buttonPressed(SW_W_PIN)) {
        if (!usbBridgeMode) LOG_SERIAL.println("[SW] White — hardware reset.");
        hardwareReset();
    }
#endif

    if (usbBridgeMode) {
        usbBridge_update();
        tcpBridge_rejectPending();
        return;
    }

#if PANEL_BUTTONS_ENABLED
    if (buttonPressed(SW_R_PIN)) {
        LOG_SERIAL.println("[SW] Red — coil sweep.");
        selfTest_coilSweep();
    }
    if (buttonPressed(SW_B_PIN)) {
        LOG_SERIAL.println("[SW] Blue — extended coil test.");
        selfTest_extended();
    }
#endif

    tcpBridge_update();
}
