// Simuliertes WiFi (STA) + WiFiClient. Modell siehe sim_runtime.cpp.
#pragma once
#include "Arduino.h"
#include <vector>

typedef enum {
    WL_NO_SHIELD = 255, WL_STOPPED = 254, WL_IDLE_STATUS = 0, WL_NO_SSID_AVAIL = 1, WL_SCAN_COMPLETED = 2,
    WL_CONNECTED = 3, WL_CONNECT_FAILED = 4, WL_CONNECTION_LOST = 5, WL_DISCONNECTED = 6
} wl_status_t;

typedef enum { WIFI_OFF = 0, WIFI_STA = 1, WIFI_AP = 2, WIFI_AP_STA = 3 } wifi_mode_t;
#define WIFI_MODE_NULL WIFI_OFF
#define WIFI_MODE_STA WIFI_STA
#define WIFI_MODE_AP WIFI_AP
#define WIFI_MODE_APSTA WIFI_AP_STA

typedef enum {
    WIFI_POWER_19_5dBm = 78, WIFI_POWER_19dBm = 76, WIFI_POWER_18_5dBm = 74, WIFI_POWER_17dBm = 68,
    WIFI_POWER_15dBm = 60, WIFI_POWER_13dBm = 52, WIFI_POWER_11dBm = 44, WIFI_POWER_8_5dBm = 34,
    WIFI_POWER_7dBm = 28, WIFI_POWER_5dBm = 20, WIFI_POWER_2dBm = 8, WIFI_POWER_MINUS_1dBm = -4
} wifi_power_t;

typedef enum { WIFI_PS_NONE, WIFI_PS_MIN_MODEM, WIFI_PS_MAX_MODEM } wifi_ps_type_t;

// Trennungsgruende (Auszug aus esp_wifi_types.h)
enum {
    WIFI_REASON_UNSPECIFIED = 1, WIFI_REASON_AUTH_EXPIRE = 2, WIFI_REASON_AUTH_LEAVE = 3, WIFI_REASON_ASSOC_EXPIRE = 4,
    WIFI_REASON_ASSOC_TOOMANY = 5, WIFI_REASON_NOT_AUTHED = 6, WIFI_REASON_NOT_ASSOCED = 7, WIFI_REASON_ASSOC_LEAVE = 8,
    WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT = 15, WIFI_REASON_GROUP_KEY_UPDATE_TIMEOUT = 16, WIFI_REASON_BEACON_TIMEOUT = 200,
    WIFI_REASON_NO_AP_FOUND = 201, WIFI_REASON_AUTH_FAIL = 202, WIFI_REASON_ASSOC_FAIL = 203,
    WIFI_REASON_HANDSHAKE_TIMEOUT = 204, WIFI_REASON_CONNECTION_FAIL = 205
};

typedef enum {
    ARDUINO_EVENT_WIFI_READY = 0, ARDUINO_EVENT_WIFI_SCAN_DONE, ARDUINO_EVENT_WIFI_STA_START, ARDUINO_EVENT_WIFI_STA_STOP,
    ARDUINO_EVENT_WIFI_STA_CONNECTED, ARDUINO_EVENT_WIFI_STA_DISCONNECTED, ARDUINO_EVENT_WIFI_STA_AUTHMODE_CHANGE,
    ARDUINO_EVENT_WIFI_STA_GOT_IP, ARDUINO_EVENT_WIFI_STA_GOT_IP6, ARDUINO_EVENT_WIFI_STA_LOST_IP, ARDUINO_EVENT_MAX
} arduino_event_id_t;
typedef arduino_event_id_t WiFiEvent_t;

typedef struct { uint8_t reason; } sim_wifi_disconnected_t;
typedef struct { struct { struct { uint32_t addr; } ip; } ip_info; } sim_got_ip_t;
typedef union {
    sim_wifi_disconnected_t wifi_sta_disconnected;
    sim_got_ip_t got_ip;
} arduino_event_info_t;
typedef arduino_event_info_t WiFiEventInfo_t;
typedef uint16_t wifi_event_id_t;
typedef void (*WiFiEventCb)(arduino_event_id_t event);
typedef std::function<void(arduino_event_id_t event, arduino_event_info_t info)> WiFiEventFuncCb;
typedef void (*WiFiEventSysCb)(void* event);

class WiFiClass {
public:
    bool mode(wifi_mode_t m) { mode_ = m; return true; }
    wifi_mode_t getMode() { return mode_; }
    bool enableSTA(bool e) { if (e) mode_ = WIFI_STA; return true; }
    bool setHostname(const char* h) { hostname_ = h ? h : ""; return true; }
    const char* getHostname() { return hostname_.c_str(); }
    bool setSleep(bool s) { sleep_ = s; return true; }
    bool setSleep(wifi_ps_type_t t) { sleep_ = t != WIFI_PS_NONE; return true; }
    bool getSleep() { return sleep_; }
    bool setAutoReconnect(bool a);
    bool getAutoReconnect();
    bool persistent(bool) { return true; }
    bool setTxPower(wifi_power_t p) { txpower_ = p; return true; }
    wifi_power_t getTxPower() { return txpower_; }
    bool config(IPAddress, IPAddress, IPAddress, IPAddress = IPAddress(), IPAddress = IPAddress()) { return true; }

    wl_status_t begin(const char* ssid, const char* pass = nullptr, int32_t channel = 0, const uint8_t* bssid = nullptr, bool connect = true);
    wl_status_t begin();
    bool disconnect(bool wifioff = false, bool eraseap = false);
    bool reconnect();
    wl_status_t status();
    bool isConnected() { return status() == WL_CONNECTED; }
    uint8_t waitForConnectResult(unsigned long timeoutLength = 60000);

    IPAddress localIP();
    IPAddress gatewayIP() { return IPAddress(192, 168, 178, 1); }
    IPAddress subnetMask() { return IPAddress(255, 255, 255, 0); }
    IPAddress dnsIP(uint8_t = 0) { return gatewayIP(); }
    String macAddress();  // pro Sim-Knoten eindeutig (aus der IP abgeleitet)
    String SSID() { return String(ssid_); }
    String BSSIDstr() { return String("11:22:33:44:55:66"); }
    int32_t channel() { return 6; }
    int8_t RSSI();
    static const char* disconnectReasonName(int reason);

    wifi_event_id_t onEvent(WiFiEventCb cb, arduino_event_id_t event = ARDUINO_EVENT_MAX);
    wifi_event_id_t onEvent(WiFiEventFuncCb cb, arduino_event_id_t event = ARDUINO_EVENT_MAX);
    void removeEvent(wifi_event_id_t id);

    int hostByName(const char* host, IPAddress& result);

    // Sim-intern
    void sim_tick();

private:
    wifi_mode_t mode_ = WIFI_OFF;
    std::string hostname_ = "esp32c3-sim";
    std::string ssid_;
    bool sleep_ = true;
    wifi_power_t txpower_ = WIFI_POWER_19_5dBm;
};
extern WiFiClass WiFi;

class WiFiClient {
public:
    WiFiClient() {}
    ~WiFiClient() { stop(); }
    WiFiClient(const WiFiClient&) = delete;
    WiFiClient& operator=(const WiFiClient&) = delete;
    int connect(IPAddress ip, uint16_t port) { return connect(ip, port, timeout_); }
    int connect(IPAddress ip, uint16_t port, int32_t timeout_ms);
    int connect(const char* host, uint16_t port) { return connect(host, port, timeout_); }
    int connect(const char* host, uint16_t port, int32_t timeout_ms);
    // arduino-esp32 2.0.x: Parameter in SEKUNDEN (setzt _timeout = seconds * 1000).
    int setTimeout(uint32_t seconds) { timeout_ = (int)(seconds * 1000); return 0; }
    void stop();
    uint8_t connected() { return fd_ >= 0; }
    operator bool() { return connected(); }
    size_t write(const uint8_t* buf, size_t n);
    size_t print(const String& s) { return write((const uint8_t*)s.c_str(), s.length()); }
    int available();
    int read();
    int fd() const { return fd_; }
private:
    int fd_ = -1;
    int timeout_ = 3000;  // WIFI_CLIENT_DEF_CONN_TIMEOUT_MS
};
