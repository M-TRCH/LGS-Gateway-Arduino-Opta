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

/*  Status lamps on outputs 2-4 — green, amber, red — as a traffic light, one
 *  at a time, highest concern first:
 *
 *    red     the shelf is not usable: a reset is running, the gateway is in
 *            safe mode or cannot store its settings, the LAN it was told to
 *            use is down, or the RS485 bus has stopped answering
 *    amber   the gateway is talking to the cabinet, or a panel sweep is running
 *    green   ready
 *
 *  Amber therefore covers a server's normal polling: while somebody is using
 *  the cabinet the panel shows amber, and green means "ready and nobody is
 *  asking". That is what the colours were asked to mean.
 */
enum PanelLamp : uint8_t {
    LAMP_GREEN = 0,
    LAMP_AMBER = 1,
    LAMP_RED   = 2,
};

const char* panel_lampName();

/*  Force a lamp on for `ms`, ignoring what the gateway's state would ask for.
 *  This is how the panel's wiring is checked at the cabinet: drive each
 *  colour in turn and watch. `lamp` is a PanelLamp, or PANEL_LAMP_OFF for
 *  all three out. Expires on its own, so a console session that walks away
 *  cannot leave the panel lying. */
#define PANEL_LAMP_OFF  0xFE
void panel_forceLamp(uint8_t lamp, uint32_t ms);

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
