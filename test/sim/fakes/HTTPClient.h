// Simulierter blockierender HTTPClient (arduino-esp32 2.0.x Semantik:
// Connect-Timeout 5000 ms Standard, setTimeout() = Lese-Timeout in ms).
#pragma once
#include "Arduino.h"
#include "WiFi.h"
#include <vector>

#define HTTPC_ERROR_CONNECTION_REFUSED (-1)
#define HTTPC_ERROR_SEND_HEADER_FAILED (-2)
#define HTTPC_ERROR_SEND_PAYLOAD_FAILED (-3)
#define HTTPC_ERROR_NOT_CONNECTED (-4)
#define HTTPC_ERROR_CONNECTION_LOST (-5)
#define HTTPC_ERROR_NO_STREAM (-6)
#define HTTPC_ERROR_NO_HTTP_SERVER (-7)
#define HTTPC_ERROR_TOO_LESS_RAM (-8)
#define HTTPC_ERROR_ENCODING (-9)
#define HTTPC_ERROR_STREAM_WRITE (-10)
#define HTTPC_ERROR_READ_TIMEOUT (-11)
#define HTTP_CODE_OK 200

class HTTPClient {
public:
    ~HTTPClient() { end(); }
    bool begin(const String& url);
    bool begin(WiFiClient&, const String& url) { return begin(url); }
    bool begin(const String& host, uint16_t port, const String& uri = "/");
    void end();
    void setTimeout(uint16_t ms) { readTimeout_ = ms; }
    void setConnectTimeout(int32_t ms) { connectTimeout_ = ms; }
    void setReuse(bool) {}
    void setUserAgent(const String&) {}
    void setAuthorization(const char* user, const char* pass);
    void addHeader(const String& name, const String& value, bool = false, bool = true);
    int GET() { return sendRequest("GET", std::string()); }
    int POST(const String& payload) { return sendRequest("POST", payload.std()); }
    int POST(uint8_t* payload, size_t size) { return sendRequest("POST", std::string((char*)payload, size)); }
    int PUT(const String& payload) { return sendRequest("PUT", payload.std()); }
    int sendRequest(const char* method, const String& payload) { return sendRequest(method, payload.std()); }
    int sendRequest(const char* method, const std::string& payload);
    String getString() { return String(body_); }
    int getSize() { return (int)body_.size(); }
    bool connected() { return false; }
    static String errorToString(int error);

private:
    std::string host_, uri_;
    int port_ = 80;
    int32_t connectTimeout_ = 5000;
    uint16_t readTimeout_ = 5000;
    std::vector<std::string> headers_;
    std::string body_;
};
