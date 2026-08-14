#include "tcp_bridge.h"
#include "event_log.h"
#include "gw_remote.h"
#include "gw_status.h"
#include "modbus_rtu.h"

// Constructed without a port: the listener does not exist until net_runtime
// starts it, so nothing here runs on a gateway with the network switched off.
static EthernetServer _server;
static EthernetClient _client[TCP_CLIENT_SLOTS];

// ── Listener lifecycle ─────────────────────────────────────────────────────
void tcpBridge_start(uint16_t port) {
    // MbedServer::begin() only allocates when its socket is null, so an
    // existing listener has to be released before it will bind a new port.
    for (int i = 0; i < TCP_CLIENT_SLOTS; i++) {
        if (_client[i]) _client[i].stop();
    }
    _server.end();
    _server.begin(port);
}

void tcpBridge_stop() {
    for (int i = 0; i < TCP_CLIENT_SLOTS; i++) {
        if (_client[i]) _client[i].stop();
    }
    _server.end();
}

bool tcpBridge_hasClient() {
    for (int i = 0; i < TCP_CLIENT_SLOTS; i++) {
        if (_client[i]) return true;
    }
    return false;
}

int tcpBridge_clientCount() {
    int n = 0;
    for (int i = 0; i < TCP_CLIENT_SLOTS; i++) {
        if (_client[i]) n++;
    }
    return n;
}

void tcpBridge_clientDesc(char* out, size_t n) {
    size_t at = 0;
    out[0] = '\0';
    for (int i = 0; i < TCP_CLIENT_SLOTS; i++) {
        if (!_client[i]) continue;
        const IPAddress ip = _client[i].remoteIP();
        at += snprintf(out + at, (at < n) ? n - at : 0, "%s%u.%u.%u.%u:%u",
                       at ? "," : "", (unsigned)ip[0], (unsigned)ip[1],
                       (unsigned)ip[2], (unsigned)ip[3],
                       (unsigned)_client[i].remotePort());
    }
    if (at == 0) snprintf(out, n, "-");
}

// ── One slot's request/response, at most one transaction per call ──────────
static void serveSlot(int slot) {
    EthernetClient& cl = _client[slot];
    if (!cl) return;

    // ── Maintain active connection ─────────────────────────────────────────
    if (!cl.connected()) {
        LOG_SERIAL.print("[NET] Client disconnected (slot ");
        LOG_SERIAL.print(slot + 1); LOG_SERIAL.println(").");
        eventLog_note(GW_EV_TCP_CLOSE, (uint8_t)(slot + 1), 0);
        cl.stop();
        return;
    }

    // ── Read MBAP Header (6 bytes) ─────────────────────────────────────────
    if (cl.available() < MBAP_HEADER_LEN) return;

    uint8_t tcp_buf[TCP_BUF_SIZE];
    if (cl.read(tcp_buf, MBAP_HEADER_LEN) != MBAP_HEADER_LEN) {
        LOG_SERIAL.println("[ERR] Failed to read MBAP header.");
        return;
    }

    uint16_t transId = ((uint16_t)tcp_buf[0] << 8) | tcp_buf[1];
    uint16_t protoId = ((uint16_t)tcp_buf[2] << 8) | tcp_buf[3];
    uint16_t mbapLen = ((uint16_t)tcp_buf[4] << 8) | tcp_buf[5];

    if (protoId != 0) {
        LOG_SERIAL.println("[ERR] Protocol ID != 0 — not Modbus TCP, discarding.");
        while (cl.available()) cl.read();
        return;
    }
    if (mbapLen < 2 || mbapLen > (TCP_BUF_SIZE - MBAP_HEADER_LEN)) {
        LOG_SERIAL.print("[ERR] MBAP Length out of range: "); LOG_SERIAL.println(mbapLen);
        while (cl.available()) cl.read();
        return;
    }

    // ── Wait for full payload (handles TCP fragmentation) ─────────────────
    unsigned long t_tcp = millis();
    while (cl.available() < (int)mbapLen) {
        if (millis() - t_tcp > TIMEOUT_TCP_PAYLOAD_MS) {
            LOG_SERIAL.print("[ERR] TCP payload timeout — got ");
            LOG_SERIAL.print(cl.available()); LOG_SERIAL.print("/"); LOG_SERIAL.println(mbapLen);
            while (cl.available()) cl.read();
            return;
        }
    }

    int n = cl.read(tcp_buf + MBAP_HEADER_LEN, (int)mbapLen);
    if (n != (int)mbapLen) {
        LOG_SERIAL.print("[ERR] Payload read mismatch: expected "); LOG_SERIAL.print(mbapLen);
        LOG_SERIAL.print(" got "); LOG_SERIAL.println(n);
        while (cl.available()) cl.read();
        return;
    }

    int tcp_total = MBAP_HEADER_LEN + n;
    gwStatus_count(GW_TCP_OK);
    printHex("TCP RX", tcp_buf, tcp_total);
    LOG_SERIAL.print("[TCP] Unit ID="); LOG_SERIAL.print(tcp_buf[6]);
    LOG_SERIAL.print("  FC=0x"); LOG_SERIAL.println(tcp_buf[7], HEX);

    // ── Unit GW_SELF_UNIT: the gateway itself — never the bus ──────────────
    // The console and the network firmware update ride here. Everything is
    // answered (exceptions included): 255 is no LGS address, and before this
    // existed a probe of it burned a full RS485 timeout for nothing.
    if (tcp_buf[6] == GW_SELF_UNIT) {
        uint8_t rpdu[TCP_BUF_SIZE];
        const int rlen = gwRemote_handle(tcp_buf + MBAP_HEADER_LEN + 1,
                                         (int)mbapLen - 1,
                                         rpdu, sizeof(rpdu), slot);
        uint8_t resp[TCP_BUF_SIZE];
        const int resp_pdu = rlen + 1;              // unit id + PDU
        resp[0] = (uint8_t)(transId >> 8);
        resp[1] = (uint8_t)(transId & 0xFF);
        resp[2] = 0x00;
        resp[3] = 0x00;
        resp[4] = (uint8_t)(resp_pdu >> 8);
        resp[5] = (uint8_t)(resp_pdu & 0xFF);
        resp[6] = GW_SELF_UNIT;
        memcpy(resp + 7, rpdu, rlen);
        printHex("TCP TX", resp, MBAP_HEADER_LEN + resp_pdu);
        cl.write(resp, MBAP_HEADER_LEN + resp_pdu);
        return;
    }

    // ── Build RTU frame: strip MBAP, append CRC ────────────────────────────
    int rtu_len = (int)mbapLen;
    if (rtu_len > (RTU_BUF_SIZE - 2)) {
        LOG_SERIAL.print("[ERR] RTU PDU length out of range: "); LOG_SERIAL.println(rtu_len);
        return;
    }
    uint8_t rtu_buf[RTU_BUF_SIZE];
    memcpy(rtu_buf, tcp_buf + MBAP_HEADER_LEN, rtu_len);
    uint16_t c = crc16(rtu_buf, rtu_len);
    rtu_buf[rtu_len]     = c & 0xFF;
    rtu_buf[rtu_len + 1] = (c >> 8) & 0xFF;
    printHex("RTU TX", rtu_buf, rtu_len + 2);

    // ── Broadcast: forward and move on (no slave answers address 0) ───────
    if (rtu_buf[0] == 0x00) {
        rtu_send(rtu_buf, rtu_len + 2);
        gwStatus_pulseRs485();
        LOG_SERIAL.println("[TCP] Broadcast forwarded — no reply expected.");
        return;
    }

    // ── RS485 transaction ──────────────────────────────────────────────────
    // Blocking, and serialized by construction: the OTHER slot simply waits
    // its turn in the same loop iteration. The bus has one wire — two truly
    // parallel transactions do not exist at this layer anyway.
    uint8_t rx_buf[RTU_BUF_SIZE];
    unsigned long t0 = millis();
    int rx_len = rtu_transact(rtu_buf, rtu_len + 2, rx_buf);
    gwStatus_pulseRs485();
    gwStatus_countRtu(rx_len > 0, millis() - t0);

    if (rx_len == 0) {
        LOG_SERIAL.println("[ERR] No response from slave — not forwarding to TCP client.");
        return;
    }
    printHex("RTU RX", rx_buf, rx_len);

    if (rx_len < 3 || !verifyCRC(rx_buf, rx_len)) {
        LOG_SERIAL.println("[ERR] CRC FAIL — discarding corrupted frame.");
        return;
    }

    // ── Wrap response in MBAP and forward to TCP client ────────────────────
    int pdu_len = rx_len - 2;   // strip the 2-byte CRC
    if (pdu_len > (TCP_BUF_SIZE - MBAP_HEADER_LEN)) {
        LOG_SERIAL.print("[ERR] PDU too large for TCP buffer: "); LOG_SERIAL.println(pdu_len);
        return;
    }

    uint8_t resp[TCP_BUF_SIZE];
    resp[0] = (uint8_t)(transId >> 8);
    resp[1] = (uint8_t)(transId & 0xFF);
    resp[2] = 0x00;
    resp[3] = 0x00;
    resp[4] = (uint8_t)(pdu_len >> 8);
    resp[5] = (uint8_t)(pdu_len & 0xFF);
    memcpy(resp + MBAP_HEADER_LEN, rx_buf, pdu_len);

    int resp_total = MBAP_HEADER_LEN + pdu_len;
    printHex("TCP TX", resp, resp_total);
    cl.write(resp, resp_total);
    LOG_SERIAL.print("[TCP] Forwarded "); LOG_SERIAL.print(resp_total); LOG_SERIAL.println(" bytes to client.");
}

// ── Update (call every loop) ───────────────────────────────────────────────
void tcpBridge_update() {
    // ── Accept into a free slot, refuse when both are taken ────────────────
    EthernetClient newClient = _server.accept();
    if (newClient) {
        int freeSlot = -1;
        for (int i = 0; i < TCP_CLIENT_SLOTS; i++) {
            if (!_client[i]) { freeSlot = i; break; }
        }
        if (freeSlot < 0) {
            LOG_SERIAL.print("[NET] Refused connection from "); LOG_SERIAL.print(newClient.remoteIP());
            LOG_SERIAL.println(" — both client slots taken.");
            eventLog_note(GW_EV_TCP_REFUSED, 0, (uint16_t)newClient.remoteIP()[3]);
            newClient.stop();
        } else {
            _client[freeSlot] = newClient;
            LOG_SERIAL.print("[NET] Client connected (slot "); LOG_SERIAL.print(freeSlot + 1);
            LOG_SERIAL.print("): "); LOG_SERIAL.print(newClient.remoteIP());
            LOG_SERIAL.print(":"); LOG_SERIAL.println(newClient.remotePort());
            eventLog_note(GW_EV_TCP_ACCEPT, (uint8_t)(freeSlot + 1),
                          (uint16_t)newClient.remoteIP()[3]);
        }
    }

    // One request per slot per iteration keeps the loop's worst case at two
    // bus transactions (~3 s against an 8 s watchdog) and neither client
    // able to starve the other.
    for (int i = 0; i < TCP_CLIENT_SLOTS; i++) serveSlot(i);
}
