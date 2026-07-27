#include "tcp_bridge.h"
#include "modbus_rtu.h"

static EthernetServer _server(MODBUS_TCP_PORT);
static EthernetClient _client;

// ── Init ───────────────────────────────────────────────────────────────────
void tcpBridge_init(byte* mac, const IPAddress& ip) {
    Ethernet.begin(mac, ip);
    _server.begin();
    LOG_SERIAL.print("[INIT] IP: "); LOG_SERIAL.println(Ethernet.localIP());
    LOG_SERIAL.print("[INIT] Modbus TCP listening on port "); LOG_SERIAL.println(MODBUS_TCP_PORT);
}

// ── Update (call every loop) ───────────────────────────────────────────────
void tcpBridge_update() {
    // ── Accept / reject new incoming connection ────────────────────────────
    EthernetClient newClient = _server.accept();
    if (newClient) {
        if (_client) {
            LOG_SERIAL.print("[NET] Refused connection from "); LOG_SERIAL.print(newClient.remoteIP());
            LOG_SERIAL.println(" — already serving a client.");
            newClient.stop();
        } else {
            _client = newClient;
            LOG_SERIAL.print("[NET] Client connected: "); LOG_SERIAL.print(_client.remoteIP());
            LOG_SERIAL.print(":"); LOG_SERIAL.println(_client.remotePort());
        }
    }

    if (!_client) return;

    // ── Maintain active connection ─────────────────────────────────────────
    if (!_client.connected()) {
        LOG_SERIAL.println("[NET] Client disconnected.");
        _client.stop();
        return;
    }

    // ── Read MBAP Header (6 bytes) ─────────────────────────────────────────
    if (_client.available() < MBAP_HEADER_LEN) return;

    uint8_t tcp_buf[TCP_BUF_SIZE];
    if (_client.read(tcp_buf, MBAP_HEADER_LEN) != MBAP_HEADER_LEN) {
        LOG_SERIAL.println("[ERR] Failed to read MBAP header.");
        return;
    }

    uint16_t transId = ((uint16_t)tcp_buf[0] << 8) | tcp_buf[1];
    uint16_t protoId = ((uint16_t)tcp_buf[2] << 8) | tcp_buf[3];
    uint16_t mbapLen = ((uint16_t)tcp_buf[4] << 8) | tcp_buf[5];

    if (protoId != 0) {
        LOG_SERIAL.println("[ERR] Protocol ID != 0 — not Modbus TCP, discarding.");
        while (_client.available()) _client.read();
        return;
    }
    if (mbapLen < 2 || mbapLen > (TCP_BUF_SIZE - MBAP_HEADER_LEN)) {
        LOG_SERIAL.print("[ERR] MBAP Length out of range: "); LOG_SERIAL.println(mbapLen);
        while (_client.available()) _client.read();
        return;
    }

    // ── Wait for full payload (handles TCP fragmentation) ─────────────────
    unsigned long t_tcp = millis();
    while (_client.available() < (int)mbapLen) {
        if (millis() - t_tcp > TIMEOUT_TCP_PAYLOAD_MS) {
            LOG_SERIAL.print("[ERR] TCP payload timeout — got ");
            LOG_SERIAL.print(_client.available()); LOG_SERIAL.print("/"); LOG_SERIAL.println(mbapLen);
            while (_client.available()) _client.read();
            return;
        }
    }

    int n = _client.read(tcp_buf + MBAP_HEADER_LEN, (int)mbapLen);
    if (n != (int)mbapLen) {
        LOG_SERIAL.print("[ERR] Payload read mismatch: expected "); LOG_SERIAL.print(mbapLen);
        LOG_SERIAL.print(" got "); LOG_SERIAL.println(n);
        while (_client.available()) _client.read();
        return;
    }

    int tcp_total = MBAP_HEADER_LEN + n;
    printHex("TCP RX", tcp_buf, tcp_total);
    LOG_SERIAL.print("[TCP] Unit ID="); LOG_SERIAL.print(tcp_buf[6]);
    LOG_SERIAL.print("  FC=0x"); LOG_SERIAL.println(tcp_buf[7], HEX);

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

    // ── RS485 transaction ──────────────────────────────────────────────────
    uint8_t rx_buf[RTU_BUF_SIZE];
    int rx_len = rtu_transact(rtu_buf, rtu_len + 2, rx_buf);

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
    _client.write(resp, resp_total);
    LOG_SERIAL.print("[TCP] Forwarded "); LOG_SERIAL.print(resp_total); LOG_SERIAL.println(" bytes to client.");
}

// ── Mode support (USB-RS485 bridge) ────────────────────────────────────────
void tcpBridge_dropClient() {
    if (_client) _client.stop();
}

void tcpBridge_rejectPending() {
    EthernetClient newClient = _server.accept();
    if (newClient) newClient.stop();
}
