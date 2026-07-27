#ifndef TCP_BRIDGE_H
#define TCP_BRIDGE_H

#include <Ethernet.h>
#include "config.h"

// Initialise Ethernet with the given MAC and static IP, then start TCP server.
void tcpBridge_init(byte* mac, const IPAddress& ip);

// Process one iteration of the Modbus TCP ↔ RTU bridge.
// Call every loop() iteration.
void tcpBridge_update();

// Drop the active client, if any. Used when leaving TCP-gateway mode.
void tcpBridge_dropClient();

// Accept and immediately close any pending connection, silently.
// Call instead of tcpBridge_update() while the USB-RS485 bridge is active.
void tcpBridge_rejectPending();

#endif // TCP_BRIDGE_H
