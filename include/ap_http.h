#ifndef AP_HTTP_H
#define AP_HTTP_H

#include <Arduino.h>
#include <Ethernet.h>

// ── The service AP's page, served on port 502 itself ───────────────────────
// docs/design-service-ap.md: lwIP has no socket left for a second listener, so tcp_bridge hands a connection to this
// when its first bytes are "GET " (a Modbus TCP frame has protocol id 0 there) AND the client is on the AP subnet.
// One request per connection, answered and closed in this call: `GET /` the page, `GET /api?l=<console line>` one
// `$LGS` line through gwConsole_execute(). `head` holds the bytes tcp_bridge already read.
void apHttp_serve(EthernetClient& cl, const uint8_t* head, int headLen);

#endif // AP_HTTP_H
