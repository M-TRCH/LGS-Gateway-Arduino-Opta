#include <Ethernet.h>
#include <ArduinoRS485.h>
#include "config.h"
#include "modbus_rtu.h"
#include "tcp_bridge.h"

// ── Network configuration (edit to suit your deployment) ──────────────────
static byte      MAC_ADDR[] = { 0xDE, 0xAD, 0xBE, 0xEF, 0xFE, 0xED };
static IPAddress STATIC_IP(192, 168, 0, 178);

// ── Hardware reset ─────────────────────────────────────────────────────────
static void hardwareReset() {
    Serial.println("[SYS] Hardware reset...");
    digitalWrite(MODULE_RELAY_PIN, LOW);
    digitalWrite(LED_RELAY_PIN,    LOW);
    delay(3000);
    NVIC_SystemReset();
}

// ── Startup coil sweep (visual self-test) ─────────────────────────────────
static void coilSweep() {
    for (int row = 1; row <= 6; row++) {
        for (int col = 1; col <= 4; col++) {
            int id = (row * 10) + col;
            writeCoil(id, 1004, true);  delay(200);
            writeCoil(id, 1004, false); delay(200);
        }
    }
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

    delay(2000);
    coilSweep();
}

// ── Loop ───────────────────────────────────────────────────────────────────
void loop() {
    // White button → hardware reset
    if (digitalRead(SW_W_PIN) == HIGH) {
        delay(50);
        if (digitalRead(SW_W_PIN) == HIGH) {
            Serial.println("[SW] White — hardware reset.");
            hardwareReset();
        }
    }

    // Red button → coil sweep test
    if (digitalRead(SW_R_PIN) == HIGH) {
        delay(50);
        if (digitalRead(SW_R_PIN) == HIGH) {
            Serial.println("[SW] Red — coil sweep.");
            coilSweep();
        }
    }

    // Blue button → extended coil test
    if (digitalRead(SW_B_PIN) == HIGH) {
        delay(50);
        if (digitalRead(SW_B_PIN) == HIGH) {
            Serial.println("[SW] Blue — extended coil test.");
            for (int row = 1; row <= 6; row++) {
                for (int col = 1; col <= 4; col++) {
                    int id = (row * 10) + col;
                    writeCoil(id, 1024, true);  delay(2000);
                    writeCoil(id, 1004, false); delay(1000);
                }
            }
        }
    }

    tcpBridge_update();
}
