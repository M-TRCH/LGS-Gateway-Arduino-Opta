#ifndef TCP_BRIDGE_H
#define TCP_BRIDGE_H

#include <Ethernet.h>
#include "config.h"

// Initialise Ethernet with the given MAC and static IP, then start TCP server.
void tcpBridge_init(byte* mac, const IPAddress& ip);

// Process one iteration of the Modbus TCP ↔ RTU bridge.
// Call every loop() iteration.
void tcpBridge_update();

#endif // TCP_BRIDGE_H
