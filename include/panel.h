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
    PANEL_LAMP_TEST  = 5,   // light the status lamps one by one (the shelf's
                            // output is never touched); the yellow button's
                            // default job — check the lamps with no PC
    PANEL_ALL_8      = 6,   // every window of every slot, for the 8-colour
                            // mask cabinets (type 80). all_on lights ONE
                            // preset per slot, which on a mask board checks
                            // one window in eight and says nothing about the
                            // other seven. Needs module fw >= 3.5.0 to mean
                            // anything: below that the windows are radio and
                            // the sweep just ends on window 8.
    PANEL_ACTION_MAX = 6,
};

// Slot count of the cabinet a sweep walks — but A COUNT IS NOT A SHAPE, and
// these three are only the shapes that happened to be built first. 40 means
// ten rows of four; the refrigerated cabinet is also forty slots and is five
// rows of eight, so it cannot be spelled with this key at all. Neither can
// the 56. Those cabinets set `panel.shape` instead.
//
// PANEL_CABINET_NONE says "there is no preset, follow panel.shape". Without
// it a shape-driven cabinet is forced to carry a preset that lies about its
// geometry — which is exactly what left Chest-Nar-01, a 7x8, claiming to be
// a 64 until 2026-09-23. A cabinet that is none of these has no known
// geometry and walks nothing, rather than silently walking 80 slots that may
// not exist.
#define PANEL_CABINET_NONE   0
#define PANEL_CABINET_40    40
#define PANEL_CABINET_64    64
#define PANEL_CABINET_80    80

#define PANEL_BUTTONS       5

/*  What lights an output. Each of outputs 2-4 is mapped to one of these
 *  (`panel.out2` / `out3` / `out4`), so the panel's colours mean whatever the
 *  site wired and decided rather than whatever the firmware assumed.
 *
 *  READY / BUSY / FAULT are the three faces of one state, so mapping them to
 *  three outputs gives a traffic light: exactly one is lit, worst first. The
 *  rest are plain facts and may be lit alongside anything.
 */
enum PanelSource : uint8_t {
    SRC_NONE     = 0,   // output left off
    SRC_READY    = 1,   // no fault, and nothing is talking
    SRC_BUSY     = 2,   // talking to the cabinet, or a sweep is running
    SRC_FAULT    = 3,   // resetting, safe mode, no store, link down, bus dead
    SRC_LINK     = 4,   // the LAN is up
    SRC_CLIENT   = 5,   // a Modbus TCP client is connected
    SRC_SWEEP    = 6,   // a panel sweep is running
    SRC_RESET    = 7,   // the shelf's power is dropped right now
    // The shelf's own power: energised except while a reset is running. This
    // is what makes a reset a reset, so it is not a lamp and never joins one:
    // the dwell does not apply to it, switching the lamps off does not switch
    // it off, and a lamp test leaves it alone.
    SRC_SHELF    = 8,
    SRC_MAX      = 8,
};

#define PANEL_OUTPUTS   4           // outputs 1-4

const char* panel_sourceName(uint8_t source);
const char* panel_lampName();       // which outputs are lit, e.g. "-2-" or "off"

/*  Force one output on for `ms`, ignoring what it is mapped to. This is how
 *  the panel's wiring is checked at the cabinet: drive each output in turn
 *  and watch which lamp answers. `out` is 1-4, or PANEL_LAMP_OFF for all
 *  off. An output carrying the shelf's power is never touched either
 *  way — a lamp test must not cut the cabinet.
 *  Expires on its own, so a console session that walks away
 *  cannot leave the panel lying. */
#define PANEL_LAMP_OFF  0xFE
void panel_forceLamp(uint8_t out, uint32_t ms);

// Drop the shelf's power for panel.reset_ms — the white button's action,
// also used by the scheduler so both are the same event.
void panel_startReset();

void panel_begin();
void panel_update();
void panel_applyConfig();           // called by gw_config on load and save

const char* panel_actionName(uint8_t action);
const char* panel_stateName();      // "idle" or the running action
uint16_t    panel_progress();       // slots done in the current sweep
uint16_t    panel_total();          // slots in the current sweep, 0 when idle
uint8_t     panel_inputMask();      // live input levels, bit0 = input 1

// The slot at `index` of a preset cabinet, or 0 past the end. Exposed for
// the console so the wiring can be checked without guessing the order.
// A cabinet with no preset (PANEL_CABINET_NONE) counts 0 slots, so both of
// these answer 0 for it and every caller's bounds check does the right thing.
uint8_t panel_slotAt(uint16_t cabinet, uint16_t index);
uint16_t panel_slotCount(uint16_t cabinet);

// What the sweeps actually walk: `panel.shape` (slots per row) when one is
// set, else `panel.cabinet`'s preset — so a cabinet whose geometry the
// catalogue does not carry still gets working front-panel buttons.
uint8_t  panel_activeSlotAt(uint16_t index);
uint16_t panel_activeSlotCount();

#endif // PANEL_H
