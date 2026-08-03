#ifndef TCP_BRIDGE_H
#define TCP_BRIDGE_H

#include <Ethernet.h>
#include "config.h"

// Modbus TCP ↔ RTU bridge. The listener is owned by net_runtime, which starts
// it once the link is up and stops it when the link drops; this module never
// touches the Ethernet interface itself.

// (Re)start the listener on `port`, dropping the current client. Safe to call
// again with a different port — the old socket is released first.
void tcpBridge_start(uint16_t port);

// Drop the client and release the listening socket.
void tcpBridge_stop();

// Process one iteration. Call every loop() iteration while the link is up.
void tcpBridge_update();

// True while a TCP client is connected (reported by the console as net.client).
bool tcpBridge_hasClient();

#endif // TCP_BRIDGE_H
