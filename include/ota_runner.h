#ifndef OTA_RUNNER_H
#define OTA_RUNNER_H

#include <Arduino.h>

// ── Module OTA run by the gateway, in the background ───────────────────────
// docs/design-gateway-ota.md. The image comes from the depot (gw_remote), the work happens on the RS485 bus in the gaps
// between everybody else's traffic, and the result is written down here. The Test Tool only starts it and reads it back
// (console verbs OTA START / STATUS / RESULT / ABORT, over USB or the unit 255 tunnel).
//
// The bus rule: the runner is stepped from loop() after the bridges, one RS485 transaction (or one timer check) per call, and
// it transmits only when rtu_msSinceForeign() says nobody else has used the bus for a while. A request on either bridge or a
// panel sweep therefore always goes first, and waits at most for one frame the runner had already started.

// Nothing to do at boot: a result file left in the "running" state is reported as interrupted the first time it is read.
void otaRunner_begin();

// Call every loop() iteration.
void otaRunner_update();

// True from START until the run finishes, is aborted, or the board resets. A gateway firmware stage must not start meanwhile.
bool otaRunner_running();

// The verbs. `armed` is the console session state (START and ABORT need it). Output goes through `emit`, one line per call,
// in the usual `#DATA ...` / `#OK ...` / `#ERR ...` form.
void otaRunner_command(char** argv, int argc, bool armed, void (*emit)(const char* line));

// One line for `INFO`, no spaces inside values.
void otaRunner_infoLine(char* out, size_t n);

#endif // OTA_RUNNER_H
