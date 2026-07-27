#ifndef SELF_TEST_H
#define SELF_TEST_H

#include <Arduino.h>
#include "config.h"

// ── Self-test routines ─────────────────────────────────────────────────────
// Both tests walk the module grid (SELFTEST_ROWS × SELFTEST_COLS) issuing
// FC05 writes over RS485. They block until finished — the TCP bridge is not
// serviced while a test is running.

// Quick visual sweep: toggles SELFTEST_COIL_PRIMARY ON/OFF on every module.
// Runs once at startup and on the Red button.
void selfTest_coilSweep();

// Extended test: SELFTEST_COIL_EXTENDED ON, dwell, SELFTEST_COIL_PRIMARY OFF.
// Runs on the Blue button.
void selfTest_extended();

#endif // SELF_TEST_H
