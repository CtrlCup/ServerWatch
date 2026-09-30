// Simuliertes mDNS: Registry-Dateien unter $SW_STATE/mdns. queryService() blockiert wie
// in arduino-esp32 2.0.x (mdns_query_ptr mit 3000 ms Timeout, max. 20 Ergebnisse).
#pragma once
#include "Arduino.h"
#include "WiFi.h"
#include <vector>

class MDNSResponder {
public:
    bool begin(const String& hostname);
    void end();
    bool setInstanceName(const String&) { return true; }
    bool addService(const char* service, const char* proto, uint16_t port);
    bool addService(const String& service, const String& proto, uint16_t port) { return addService(service.c_str(), proto.c_str(), port); }
    bool addServiceTxt(const char* service, const char* proto, const char* key, const char* value);
    bool addServiceTxt(const String& service, const String& proto, const String& key, const String& value) {
        return addServiceTxt(service.c_str(), proto.c_str(), key.c_str(), value.c_str());
    }
    void enableArduino(uint16_t = 3232, bool = false) {}
    int queryService(const char* service, const char* proto);
    int queryService(const String& service, const String& proto) { return queryService(service.c_str(), proto.c_str()); }
    String hostname(int idx);
    IPAddress IP(int idx);
    uint16_t port(int idx);
    int numTxt(int idx);
    bool hasTxt(int idx, const char* key);
    String txt(int idx, const char* key);
    String txt(int idx, int txtIdx);
    String txtKey(int idx, int txtIdx);

    // Sim-intern: tatsaechlich registrierter Hostname (kann nach Konflikt "-2" enthalten)
    std::string sim_hostname() const { return host_; }

private:
    struct Result { std::string host, ip; int port; std::vector<std::pair<std::string, std::string>> txt; };
    void persist();
    std::string host_;
    std::vector<std::string> services_;  // "_svc._tcp:port"
    std::vector<std::string> txts_;      // "_svc._tcp:key=value"
    std::vector<Result> results_;
    bool started_ = false;
};
extern MDNSResponder MDNS;
