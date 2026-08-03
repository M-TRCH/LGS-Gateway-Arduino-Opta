#ifndef NET_RUNTIME_H
#define NET_RUNTIME_H

#include <Arduino.h>
#include <Ethernet.h>

// ── Ethernet lifecycle ─────────────────────────────────────────────────────
// The network is strictly optional. It is brought up only after the USB bridge
// is already serving, with a bounded begin(), and from then on it is *polled* —
// never begun again — so nothing here can stall the RTU path.
//
// Ethernet.begin() starts the interface non-blocking and then waits for the
// link for at most net.link_timeout_ms. When the cable is out it returns 0 but
// leaves the interface connecting in the background, which is what lets
// netRuntime_update() pick up a cable plugged in later without a reboot.

enum class NetState : uint8_t {
    DISABLED,   // net.enabled = 0
    SAFE,       // safe mode — the network is skipped on purpose
    NOLINK,     // configured, but the interface is not up (cable out?)
    UP,         // link up and the Modbus TCP server is listening
};

void        netRuntime_begin();
void        netRuntime_update();
bool        netRuntime_isUp();
NetState    netRuntime_state();
const char* netRuntime_stateName();
IPAddress   netRuntime_localIp();

// Move the listener to a new port. No-op unless the link is up — on a link-less
// gateway the port is simply picked up the next time the server starts.
void netRuntime_applyPort(uint16_t port);

#endif // NET_RUNTIME_H
