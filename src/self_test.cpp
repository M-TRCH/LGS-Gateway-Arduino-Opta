#include "self_test.h"
#include "modbus_rtu.h"

// ── Slave addressing ───────────────────────────────────────────────────────
// Modules are addressed by grid position: slave ID = row*10 + col
// (row 2, col 3 → slave 23).
static uint8_t slaveIdFor(int row, int col) {
    return (uint8_t)((row * 10) + col);
}

// ── Coil sweep (startup + Red button) ──────────────────────────────────────
void selfTest_coilSweep() {
    for (int row = 1; row <= SELFTEST_ROWS; row++) {
        for (int col = 1; col <= SELFTEST_COLS; col++) {
            uint8_t id = slaveIdFor(row, col);
            writeCoil(id, SELFTEST_COIL_PRIMARY, true);  delay(SELFTEST_SWEEP_STEP_MS);
            writeCoil(id, SELFTEST_COIL_PRIMARY, false); delay(SELFTEST_SWEEP_STEP_MS);
        }
    }
}

// ── Extended coil test (Blue button) ───────────────────────────────────────
void selfTest_extended() {
    for (int row = 1; row <= SELFTEST_ROWS; row++) {
        for (int col = 1; col <= SELFTEST_COLS; col++) {
            uint8_t id = slaveIdFor(row, col);
            // Intentional asymmetry: ON uses the extended coil, OFF the primary.
            writeCoil(id, SELFTEST_COIL_EXTENDED, true);  delay(SELFTEST_EXT_ON_MS);
            writeCoil(id, SELFTEST_COIL_PRIMARY,  false); delay(SELFTEST_EXT_OFF_MS);
        }
    }
}
