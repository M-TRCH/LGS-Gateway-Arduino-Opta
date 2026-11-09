#ifndef MOD_SWEEP_H
#define MOD_SWEEP_H

#include <Arduino.h>

// ── SWEEP: walk the cabinet's modules, one read per loop() pass ────────────
// For the service AP's page (and any console): which slots answer, how fast, on which firmware. Read-only (FC03 regs
// 0-1), no HELLO. One retry per module, because the first frame after a hub channel change is lost by design. Its frames
// are ordinary foreign traffic, so the background module update yields to it.
//   $LGS SWEEP START [ids=11-18,21|all]   $LGS SWEEP STATUS   $LGS SWEEP STOP
void modSweep_update();
bool modSweep_running();
void modSweep_command(char** argv, int argc, void (*emit)(const char* line));
// $LGS MOD <id> LIGHT|UNLOCK|OFF (HELLO required): one coil to one module.
void modCommand(char** argv, int argc, void (*emit)(const char* line));

#endif // MOD_SWEEP_H
