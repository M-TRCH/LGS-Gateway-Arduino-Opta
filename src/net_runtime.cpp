#include "net_runtime.h"

#include "gw_config.h"
#include "gw_status.h"
#include "tcp_bridge.h"

#define LINK_POLL_MS        500UL
#define DHCP_RESPONSE_MS    4000UL

static NetState      _state = NetState::DISABLED;
static unsigned long _nextPollMs;
static uint16_t      _listenPort;

static IPAddress toIp(uint32_t v) {
    return IPAddress((uint8_t)(v >> 24), (uint8_t)(v >> 16), (uint8_t)(v >> 8), (uint8_t)v);
}

static void setState(NetState s) {
    if (s == _state) return;
    _state = s;
    digitalWrite(LED_LINK_PIN, s == NetState::UP ? HIGH : LOW);
    LOG_SERIAL.print("[NET] state="); LOG_SERIAL.println(netRuntime_stateName());
}

static void startServer() {
    _listenPort = gwConfig_active().net_port;
    tcpBridge_start(_listenPort);
    LOG_SERIAL.print("[NET] listening on "); LOG_SERIAL.print(Ethernet.localIP());
    LOG_SERIAL.print(":"); LOG_SERIAL.println(_listenPort);
}

// The interface reports the MAC it actually uses, which is the only one worth
// showing: when the OTP read fails we hand begin() a nullptr and mbed derives a
// per-board address of its own.
static void adoptEffectiveMac() {
    String s = Ethernet.macAddress();
    unsigned b[6];
    if (sscanf(s.c_str(), "%x:%x:%x:%x:%x:%x",
               &b[0], &b[1], &b[2], &b[3], &b[4], &b[5]) != 6) return;
    uint8_t mac[6];
    for (int i = 0; i < 6; i++) mac[i] = (uint8_t)b[i];
    gwStatus_setMac(mac);
}

void netRuntime_begin() {
    const GwConfig& c = gwConfig_active();

    // Safe mode exists to get a misconfigured unit back, so it never touches
    // the network — a bad net.ip cannot lock the gateway out of its own boot.
    if (gwStatus_safeMode()) { setState(NetState::SAFE);     return; }
    if (!c.net_enabled)      { setState(NetState::DISABLED); return; }

    uint8_t mac[6];
    memcpy(mac, gwStatus_mac(), sizeof(mac));
    uint8_t* macArg = gwStatus_macValid() ? mac : nullptr;

    const int ok = c.net_dhcp
        ? Ethernet.begin(macArg, c.net_link_timeout_ms, DHCP_RESPONSE_MS)
        : Ethernet.begin(macArg, toIp(c.net_ip), toIp(c.net_dns), toIp(c.net_gw),
                         toIp(c.net_mask), c.net_link_timeout_ms, DHCP_RESPONSE_MS);

    adoptEffectiveMac();

    // A failed begin() is not an error here: the interface keeps trying in the
    // background and netRuntime_update() will notice when the link arrives.
    if (ok) {
        setState(NetState::UP);
        startServer();
    } else {
        setState(NetState::NOLINK);
        LOG_SERIAL.println("[NET] no link at boot — will pick it up when the cable arrives");
    }
    _nextPollMs = millis() + LINK_POLL_MS;
}

void netRuntime_update() {
    if (_state == NetState::DISABLED || _state == NetState::SAFE) return;
    if ((long)(millis() - _nextPollMs) < 0) return;
    _nextPollMs = millis() + LINK_POLL_MS;

    const bool up = (Ethernet.linkStatus() == LinkON);
    if (up && _state != NetState::UP) {
        setState(NetState::UP);
        adoptEffectiveMac();
        startServer();
    } else if (!up && _state == NetState::UP) {
        // Release the socket rather than hold a dead one: startServer() builds
        // a fresh listener when the link comes back.
        setState(NetState::NOLINK);
        tcpBridge_stop();
    }
}

void netRuntime_applyPort(uint16_t port) {
    if (_state != NetState::UP || port == _listenPort) return;
    _listenPort = port;
    tcpBridge_start(port);
    LOG_SERIAL.print("[NET] moved listener to port "); LOG_SERIAL.println(port);
}

bool     netRuntime_isUp() { return _state == NetState::UP; }
NetState netRuntime_state() { return _state; }

const char* netRuntime_stateName() {
    switch (_state) {
        case NetState::UP:       return "up";
        case NetState::NOLINK:   return "nolink";
        case NetState::SAFE:     return "safe";
        case NetState::DISABLED: default: return "disabled";
    }
}

IPAddress netRuntime_localIp() {
    if (_state == NetState::DISABLED || _state == NetState::SAFE) return IPAddress(0, 0, 0, 0);
    return Ethernet.localIP();
}
