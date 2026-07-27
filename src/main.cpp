#include <Ethernet.h>
#include <ArduinoRS485.h>
#include "config.h"
#include "tcp_bridge.h"
#include "self_test.h"

// ── Network identity (values in config.h) ──────────────────────────────────
static byte      MAC_ADDR[] = NET_MAC_ADDRESS;
static IPAddress STATIC_IP(NET_STATIC_IP);

// ── Hardware reset ─────────────────────────────────────────────────────────
static void hardwareReset() {
    Serial.println("[SYS] Hardware reset...");
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

// ── Setup ──────────────────────────────────────────────────────────────────
void setup() {
    Serial.begin(SERIAL_BAUD);
    delay(500);
    Serial.println("========================================");
    Serial.println("  Modbus TCP-RTU Gateway");
    Serial.println("  Board: Arduino Opta");
    Serial.println("========================================");

    pinMode(MODULE_RELAY_PIN, OUTPUT); digitalWrite(MODULE_RELAY_PIN, HIGH);
    pinMode(LED_RELAY_PIN,    OUTPUT); digitalWrite(LED_RELAY_PIN,    HIGH);
    pinMode(SW_R_PIN, INPUT);
    pinMode(SW_G_PIN, INPUT);
    pinMode(SW_B_PIN, INPUT);
    pinMode(SW_Y_PIN, INPUT);
    pinMode(SW_W_PIN, INPUT);

    Serial.println("[INIT] Configuring RS485...");
    RS485.setDelays(RS485_PRE_DELAY_US, RS485_POST_DELAY_US);
    RS485.begin(RS485_BAUD);
    RS485.receive();
    Serial.print("[INIT] RS485 ready @ "); Serial.print(RS485_BAUD); Serial.println(" baud");

    tcpBridge_init(MAC_ADDR, STATIC_IP);

    Serial.println("[INIT] Gateway ONLINE — waiting for connections...");
    Serial.println("========================================");

    delay(STARTUP_SWEEP_DELAY_MS);
    selfTest_coilSweep();
}

// ── Loop ───────────────────────────────────────────────────────────────────
void loop() {
    if (buttonPressed(SW_W_PIN)) {
        Serial.println("[SW] White — hardware reset.");
        hardwareReset();
    }
    if (buttonPressed(SW_R_PIN)) {
        Serial.println("[SW] Red — coil sweep.");
        selfTest_coilSweep();
    }
    if (buttonPressed(SW_B_PIN)) {
        Serial.println("[SW] Blue — extended coil test.");
        selfTest_extended();
    }

    tcpBridge_update();
}
