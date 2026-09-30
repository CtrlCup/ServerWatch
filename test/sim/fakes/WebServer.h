// Simulierter synchroner ESP32-WebServer: ein Client pro handleClient(), blockierend,
// wie arduino-esp32 2.0.x. Echte TCP-Sockets auf <SW_IP>:<port+SW_PORT_OFFSET>.
#pragma once
#include "Arduino.h"
#include "WiFi.h"
#include <map>
#include <vector>

typedef enum { HTTP_GET = 1, HTTP_POST = 3, HTTP_PUT = 4, HTTP_PATCH = 29, HTTP_DELETE = 0, HTTP_OPTIONS = 6, HTTP_HEAD = 2 } HTTPMethod;
#define HTTP_ANY (HTTPMethod)(255)
#define HTTP_MAX_DATA_WAIT 5000
#define HTTP_MAX_POST_WAIT 5000
#define HTTP_MAX_SEND_WAIT 5000
#define HTTP_MAX_CLOSE_WAIT 2000
#define CONTENT_LENGTH_UNKNOWN ((size_t)-1)
#define CONTENT_LENGTH_NOT_SET ((size_t)-2)
enum HTTPAuthMethod { BASIC_AUTH, DIGEST_AUTH };

class SimRemoteClient {
public:
    IPAddress remoteIP() const { return ip_; }
    IPAddress ip_;
};

class WebServer {
public:
    typedef std::function<void(void)> THandlerFunction;
    explicit WebServer(int port = 80) : port_(port) {}
    WebServer(IPAddress, int port = 80) : port_(port) {}
    ~WebServer() { close(); }

    void begin();
    void begin(uint16_t port) { port_ = port; begin(); }
    void close();
    void stop() { close(); }
    void handleClient();

    void on(const String& uri, THandlerFunction fn) { on(uri, HTTP_ANY, fn); }
    void on(const String& uri, HTTPMethod m, THandlerFunction fn) { routes_.push_back({uri, m, fn}); }
    void on(const String& uri, HTTPMethod m, THandlerFunction fn, THandlerFunction) { on(uri, m, fn); }
    void onNotFound(THandlerFunction fn) { notFound_ = fn; }

    String uri() const { return String(uri_); }
    HTTPMethod method() const { return method_; }
    String arg(const String& name) const;
    String arg(int i) const { return i < (int)args_.size() ? String(args_[i].second) : String(); }
    String argName(int i) const { return i < (int)args_.size() ? String(args_[i].first) : String(); }
    int args() const { return (int)args_.size(); }
    bool hasArg(const String& name) const;
    String header(const String& name) const;
    bool hasHeader(const String& name) const;
    String header(int i) const { return i < (int)headers_.size() ? String(headers_[i].second) : String(); }
    int headers() const { return (int)headers_.size(); }
    void collectHeaders(const char* keys[], size_t count) { collect_.assign(keys, keys + count); }
    String hostHeader() const;
    SimRemoteClient& client() { return client_; }

    bool authenticate(const char* user, const char* pass);
    void requestAuthentication(HTTPAuthMethod mode = BASIC_AUTH, const char* realm = nullptr, const String& failMsg = String(""));

    void send(int code, const char* type = nullptr, const String& content = String(""));
    void send(int code, const char* type, const char* content) { send(code, type, String(content)); }
    void send(int code, const String& type, const String& content) { send(code, type.c_str(), content); }
    void send_P(int code, PGM_P type, PGM_P content) { send(code, type, String(content)); }
    void send_P(int code, PGM_P type, PGM_P content, size_t len) { send(code, type, String(std::string(content, len))); }
    void sendHeader(const String& name, const String& value, bool first = false);
    void setContentLength(size_t len) { contentLength_ = len; }
    void sendContent(const String& content) { body_ += content.std(); }
    void sendContent(const char* content) { body_ += content; }
    void sendContent_P(PGM_P content) { body_ += content; }
    void enableCORS(bool e = true) { cors_ = e; }

private:
    struct Route { String uri; HTTPMethod method; THandlerFunction fn; };
    bool readRequest(int fd);
    bool collected(const std::string& name) const;
    std::vector<std::string> collect_;
    void flushResponse(int fd);

    int port_;
    int listenFd_ = -1;
    std::vector<Route> routes_;
    THandlerFunction notFound_;
    bool cors_ = false;

    // aktueller Request
    std::string uri_;
    HTTPMethod method_ = HTTP_GET;
    std::vector<std::pair<std::string, std::string>> args_;
    std::vector<std::pair<std::string, std::string>> headers_;
    SimRemoteClient client_;
    // Antwort
    bool responded_ = false;
    int code_ = 0;
    std::string type_;
    std::vector<std::string> respHeaders_;
    std::string body_;
    size_t contentLength_ = CONTENT_LENGTH_NOT_SET;
};
