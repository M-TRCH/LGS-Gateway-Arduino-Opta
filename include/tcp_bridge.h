#ifndef TCP_BRIDGE_H
#define TCP_BRIDGE_H

#include <Ethernet.h>
#include "config.h"

// Modbus TCP ↔ RTU bridge. The listener is owned by net_runtime, which starts
// it once the link is up and stops it when the link drops; this module never
// touches the Ethernet interface itself.
//
// TWO client slots, so the hospital's server can stay connected while a
// technician's Test Tool joins. Exactly two, not "some": lwIP on this board
// is compiled for 4 sockets total (it lives inside the prebuilt libmbed.a),
// and listener + two clients + the SNTP client's transient UDP socket is
// all four. A third connection is refused, as the second used to be.
#define TCP_CLIENT_SLOTS 2

// (Re)start the listener on `port`, dropping the current clients. Safe to
// call again with a different port — the old socket is released first.
void tcpBridge_start(uint16_t port);

// Drop the clients and release the listening socket.
void tcpBridge_stop();

// Process one iteration. Call every loop() iteration while the link is up.
void tcpBridge_update();

// True while at least one TCP client is connected.
bool tcpBridge_hasClient();
int  tcpBridge_clientCount();

// "ip:port,ip:port", "ip:port", or "-" — console-safe (never a space).
void tcpBridge_clientDesc(char* out, size_t n);

// When the bus gives no reply: stay silent (default, the contract so far) or
// answer the client at once with Modbus exception 0x0B (gateway target failed
// to respond), so it learns in milliseconds instead of at its own timeout.
// Volatile (`$LGS EXC 0|1`), off at every boot: it changes what the server
// sees, so it is switched on per site by agreement, and a stored key would
// be a config schema bump.
void tcpBridge_setException(bool on);
bool tcpBridge_exceptionEnabled();

#endif // TCP_BRIDGE_H
