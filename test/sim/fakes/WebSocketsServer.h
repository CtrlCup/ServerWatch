// Simulierter WebSocketsServer (links2004/WebSockets 2.x API-Auszug) mit echtem
// RFC6455-Handshake/Framing, damit Tests mit echten WebSocket-Clients arbeiten koennen.
#pragma once
#include "Arduino.h"
#include "WiFi.h"
#include <vector>

#ifndef WEBSOCKETS_SERVER_CLIENT_MAX
#define WEBSOCKETS_SERVER_CLIENT_MAX (5)
#endif
#ifndef WEBSOCKETS_TCP_TIMEOUT
#define WEBSOCKETS_TCP_TIMEOUT (5000)
#endif

typedef enum {
    WStype_ERROR, WStype_DISCONNECTED, WStype_CONNECTED, WStype_TEXT, WStype_BIN, WStype_FRAGMENT_TEXT_START,
    WStype_FRAGMENT_BIN_START, WStype_FRAGMENT, WStype_FRAGMENT_FIN, WStype_PING, WStype_PONG,
} WStype_t;

class WebSocketsServer {
public:
    typedef std::function<void(uint8_t num, WStype_t type, uint8_t* payload, size_t length)> WebSocketServerEvent;
    typedef std::function<bool(String headerName, String headerValue)> WebSocketServerHttpHeaderValFunc;

    WebSocketsServer(uint16_t port, const String& origin = "", const String& protocol = "arduino") : port_(port) {}
    ~WebSocketsServer();
    void begin();
    void close();
    void loop();
    void onEvent(WebSocketServerEvent cb) { cb_ = cb; }
    void onValidateHttpHeader(WebSocketServerHttpHeaderValFunc fn, const char* mandatory[] = nullptr, size_t count = 0) { validate_ = fn; }

    bool sendTXT(uint8_t num, const uint8_t* payload, size_t length = 0, bool headerToPayload = false);
    bool sendTXT(uint8_t num, const char* payload, size_t length = 0, bool headerToPayload = false) { return sendTXT(num, (const uint8_t*)payload, length ? length : strlen(payload)); }
    bool sendTXT(uint8_t num, String& payload) { return sendTXT(num, (const uint8_t*)payload.c_str(), payload.length()); }
    bool sendTXT(uint8_t num, const String& payload) { return sendTXT(num, (const uint8_t*)payload.c_str(), payload.length()); }
    bool broadcastTXT(const uint8_t* payload, size_t length = 0, bool headerToPayload = false);
    bool broadcastTXT(const char* payload, size_t length = 0, bool headerToPayload = false) { return broadcastTXT((const uint8_t*)payload, length ? length : strlen(payload)); }
    bool broadcastTXT(String& payload) { return broadcastTXT((const uint8_t*)payload.c_str(), payload.length()); }
    bool broadcastTXT(const String& payload) { return broadcastTXT((const uint8_t*)payload.c_str(), payload.length()); }
    bool sendPing(uint8_t num) { return sendFrame(num, 0x9, nullptr, 0); }
    bool broadcastPing() { for (int i = 0; i < WEBSOCKETS_SERVER_CLIENT_MAX; i++) sendPing(i); return true; }
    void disconnect();
    void disconnect(uint8_t num);
    void enableHeartbeat(uint32_t pingInterval, uint32_t pongTimeout, uint8_t disconnectTimeoutCount);
    void disableHeartbeat() { hbInterval_ = 0; }
    int connectedClients(bool ping = false);
    bool clientIsConnected(uint8_t num) { return num < WEBSOCKETS_SERVER_CLIENT_MAX && clients_[num].state == 2; }
    IPAddress remoteIP(uint8_t num);

private:
    struct Client {
        int fd = -1;
        int state = 0;  // 0 frei, 1 Header, 2 verbunden
        std::string in;
        IPAddress ip;
        double lastPing = 0, lastPong = 0;
        int missedPongs = 0;
        bool pingPending = false;
    };
    bool sendFrame(uint8_t num, uint8_t opcode, const uint8_t* data, size_t len);
    void dropClient(uint8_t num, bool notify);
    void handleHeader(uint8_t num);
    void handleFrames(uint8_t num);

    uint16_t port_;
    int listenFd_ = -1;
    Client clients_[WEBSOCKETS_SERVER_CLIENT_MAX];
    WebSocketServerEvent cb_;
    WebSocketServerHttpHeaderValFunc validate_;
    uint32_t hbInterval_ = 0, hbTimeout_ = 0;
    uint8_t hbCount_ = 0;
};
