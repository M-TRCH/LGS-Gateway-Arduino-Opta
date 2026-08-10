#ifndef PANEL_H
#define PANEL_H

#include <Arduino.h>
#include "gw_config.h"

/*  Front-panel test buttons.
 *
 *  Five buttons wired to the Opta's inputs 1-5 — red, green, blue, yellow,
 *  white — so the cabinet can be exercised at the cabinet, with no PC and no
 *  network. Every button's job is configurable (`panel.btn1`..`panel.btn5`),
 *  because which colour means what is a site decision, not a firmware one.
 *
 *  The sweeps walk the cabinet one slot at a time from loop(), never in a
 *  blocking burst: eighty slots is several seconds of Modbus, and the USB
 *  bridge, the TCP bridge and the watchdog all have to keep running through
 *  it. Pressing a button while a sweep is running replaces it, so a mistaken
 *  press is undone by pressing the one you meant.
 */

enum PanelAction : uint8_t {
    PANEL_NONE       = 0,   // button does nothing
    PANEL_ALL_ON     = 1,   // ring + number, slot by slot, and left on
    PANEL_ALL_OFF    = 2,   // everything out
    PANEL_ALL_UNLOCK = 3,   // ring + number + latch, slot by slot
    PANEL_RESET      = 4,   // drop both relays: a power cycle of the shelf
    PANEL_ACTION_MAX = 4,
};

// Slot count of the cabinet a sweep walks. Anything else is treated as 80.
#define PANEL_CABINET_40    40
#define PANEL_CABINET_64    64
#define PANEL_CABINET_80    80

#define PANEL_BUTTONS       5

void panel_begin();
void panel_update();
void panel_applyConfig();           // called by gw_config on load and save

const char* panel_actionName(uint8_t action);
const char* panel_stateName();      // "idle" or the running action
uint16_t    panel_progress();       // slots done in the current sweep
uint16_t    panel_total();          // slots in the current sweep, 0 when idle
uint8_t     panel_inputMask();      // live input levels, bit0 = input 1

// The slot at `index` of a cabinet, or 0 past the end. Exposed for the
// console so the wiring can be checked without guessing the order.
uint8_t panel_slotAt(uint16_t cabinet, uint16_t index);
uint16_t panel_slotCount(uint16_t cabinet);

#endif // PANEL_H
