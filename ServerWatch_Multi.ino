#include <WiFi.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <ArduinoJson.h>
#include <WebSocketsServer.h>
#include <HTTPClient.h>
#include <WiFiClient.h>
#include <esp_task_wdt.h>
#include <esp_idf_version.h>
#include <vector>
#include <map>
#include <algorithm>

// ========================================
// KONFIGURATIONSVARIABLEN - BITTE ANPASSEN
// ========================================

// WiFi Zugangsdaten
const char* ssid = "DEIN_WLAN_NAME";              // WLAN SSID eintragen
const char* password = "DEIN_WLAN_PASSWORT";      // WLAN Passwort eintragen

// Server Konfiguration
const char* serverName = "Heimserver";            // Name deines Servers
const char* serverIP = "192.168.178.1";           // IP-Adresse des zu überwachenden Servers
const int serverCheckPort = 80;                   // Port für Server-Check (80=HTTP, 22=SSH, 445=SMB)

// GPIO Pin Konfiguration
const int POWER_CHECK_PIN = 4;                    // Pin zum Prüfen der Spannung vom Server
const int POWER_BUTTON_PIN = 3;                   // Pin zum Server Ein/Ausschalten
const int RESET_BUTTON_PIN = 5;                   // Pin zum Server Reset (optional, -1 wenn nicht verwendet)
const bool usePowerSense = true;                  // false, wenn POWER_CHECK_PIN nicht mit dem Mainboard verbunden ist

// Timing Konfiguration
const int powerButtonTime = 800;                  // Millisekunden für Power-Button Druck
const int resetButtonTime = 500;                  // Millisekunden für Reset-Button Druck
const int scanInterval = 10000;                   // Millisekunden zwischen Netzwerk-Scans (10 Sekunden)
const int statusCheckInterval = 3000;             // Millisekunden zwischen Status-Checks (3 Sekunden)
const int pingTimeout = 1000;                     // Millisekunden Timeout für Ping-Versuche
const int watchdogTimeoutS = 15;                  // Sekunden bis zum automatischen Neustart bei Hänger
const unsigned long wifiRestartTimeout = 300000;  // Neustart, wenn WLAN so lange (ms) getrennt bleibt
const unsigned long wifiBootTimeout = 20000;      // So lange (ms) wartet setup() auf das WLAN

// mDNS Service Name
const char* mdnsServiceName = "serverwatch";      // mDNS Service Name für Auto-Discovery

// ========================================
// ENDE DER KONFIGURATIONSVARIABLEN
// ========================================

const char* firmwareVersion = "1.0.10";

// Webserver und WebSocket
WebServer server(80);
WebSocketsServer webSocket = WebSocketsServer(81);

// Remote ESP Struktur (Schlüssel in remoteESPs ist die eindeutige id)
struct RemoteESP {
    String id;             // MAC-Adresse ohne Doppelpunkte, eindeutig pro ESP
    String hostname;
    String ip;
    String serverName;
    String serverIP;
    int serverPort;
    bool hasReset;
    String version;
    bool serverOnline;
    bool serverPower;
    bool powerSense;
    bool espReachable;
    unsigned long lastSeen;
    unsigned long uptime;
    int rssi;
    int pingTime;
};

// Ergebnis eines Steuerbefehls: HTTP-Code (200 = ausgeführt) und Fehlertext
struct ActionResult {
    int code;
    String error;
};

// Globale Variablen
// Status-Check und ESP-Scan laufen in einer eigenen Task (monitorTask), damit loop()
// Webserver und WebSocket nie blockiert (Issues #2, #3). remoteESPs ist durch
// remoteMutex geschützt; die lokalen Statuswerte sind einfache volatile Variablen.
std::map<String, RemoteESP> remoteESPs;
SemaphoreHandle_t remoteMutex;
unsigned long lastStatusBroadcast = 0;
unsigned long lastWiFiCheck = 0;
const unsigned long wifiCheckInterval = 10000;
bool wifiLost = false;
unsigned long wifiLostSince = 0;
volatile bool localServerStatus = false;
volatile bool localServerReachable = false;
volatile bool localPowerStatus = false;
volatile int localPingTime = 0;
String espHostname;
String nodeId;                                    // eindeutige ID dieses ESP (MAC ohne Doppelpunkte)
const unsigned long remoteForgetTimeout = 86400000;  // nicht erreichbare ESPs nach 24 h vergessen

// HTML-Oberfläche: erzeugt aus serverwatch_Multi_interface.html (tools/embed_html.py, Issue #21)
#include "dashboard_html.h"

// Forward Deklarationen
void setupWiFi();
void setupMDNS();
void setupWebServer();
void setupWebSocket();
void scanForESPs();
void monitorTask(void* param);
void setupWatchdog();
void updateLocalStatus();
bool checkServerReachable();
void sendStatusToClients();
String getStatusJson(const char* type = nullptr);

// Text aus fremden Quellen bereinigen: Steuerzeichen entfernen, Länge begrenzen (UTF-8-sicher)
String sanitizeText(const String& text, unsigned int maxLen) {
    String out;
    for (unsigned int i = 0; i < text.length(); i++) {
        unsigned char c = text[i];
        if (c >= 0x20 && c != 0x7f) out += (char)c;
    }
    if (out.length() > maxLen) {
        unsigned int cut = maxLen;
        while (cut > 0 && ((unsigned char)out[cut] & 0xC0) == 0x80) cut--;  // kein halbes UTF-8-Zeichen
        out = out.substring(0, cut);
    }
    return out;
}

// Hostname nach RFC 1123: nur a-z, 0-9 und '-', Umlaute umschreiben, MAC-Suffix für Eindeutigkeit
String makeHostname(const String& name, const String& id) {
    String base = name;
    base.toLowerCase();
    base.replace("ä", "ae"); base.replace("ö", "oe"); base.replace("ü", "ue");
    base.replace("Ä", "ae"); base.replace("Ö", "oe"); base.replace("Ü", "ue"); base.replace("ß", "ss");
    String clean;
    for (unsigned int i = 0; i < base.length() && clean.length() < 20; i++) {
        char c = base[i];
        bool ok = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');
        if (ok) clean += c;
        else if (clean.length() > 0 && clean[clean.length() - 1] != '-') clean += '-';
    }
    while (clean.endsWith("-")) clean.remove(clean.length() - 1);
    String host = "serverwatch-";
    if (clean.length() > 0) host += clean + "-";
    return host + id.substring(id.length() - 6);
}

// WiFi Setup
void setupWiFi() {
    Serial.println("\n=== ServerWatch Multi ESP32 ===");
    Serial.print("Verbinde mit WiFi: ");
    Serial.println(ssid);
    
    // Eindeutiger Hostname aus Servername und MAC, z. B. "serverwatch-heimserver-a1b2c3" (Issue #14)
    nodeId = WiFi.macAddress();
    nodeId.replace(":", "");
    nodeId.toLowerCase();
    espHostname = makeHostname(serverName, nodeId);
    WiFi.setHostname(espHostname.c_str());
    
    // Stellt sicher, dass das WiFi-Modul nicht in den Schlafmodus geht (wichtig für Fritzboxen)
    WiFi.setSleep(false);
    WiFi.setAutoReconnect(true);
    
    WiFi.begin(ssid, password);
    
    // Nicht endlos warten: klappt es nicht, übernimmt der Reconnect im loop() (Issue #5)
    unsigned long wifiStart = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - wifiStart < wifiBootTimeout) {
        delay(500);
        Serial.print(".");
    }
    
    if (WiFi.status() == WL_CONNECTED) {
        Serial.println("\nWiFi verbunden!");
        Serial.print("IP-Adresse: ");
        Serial.println(WiFi.localIP());
    } else {
        Serial.println("\nWiFi noch nicht verbunden - neuer Versuch im Hintergrund");
    }
    Serial.print("Hostname: ");
    Serial.println(espHostname);
}

// mDNS Setup für Service Discovery
void setupMDNS() {
    if (!MDNS.begin(espHostname.c_str())) {
        Serial.println("Fehler beim mDNS Setup!");
        return;
    }
    
    // Service advertisen
    MDNS.addService(mdnsServiceName, "tcp", 80);
    MDNS.addServiceTxt(mdnsServiceName, "tcp", "server", serverName);
    MDNS.addServiceTxt(mdnsServiceName, "tcp", "id", nodeId.c_str());
    MDNS.addServiceTxt(mdnsServiceName, "tcp", "version", firmwareVersion);
    
    Serial.println("mDNS Service gestartet: " + espHostname);
}

// Server Ping Check
bool checkServerReachable() {
    WiFiClient client;

    unsigned long startTime = millis();
    // Timeout explizit in ms: setTimeout() erwartet in arduino-esp32 2.x Sekunden (Issue #1)
    bool connected = client.connect(serverIP, serverCheckPort, pingTimeout);
    
    if (connected) {
        localPingTime = millis() - startTime;
        client.stop();
        return true;
    }
    
    localPingTime = 0;
    return false;
}

// Status Update für lokalen Server
void updateLocalStatus() {
    bool reachable = checkServerReachable();
    localServerReachable = reachable;
    // Ohne Spannungsabgriff zählt allein die Erreichbarkeit (Issue #19)
    localPowerStatus = usePowerSense && digitalRead(POWER_CHECK_PIN) == HIGH;
    localServerStatus = usePowerSense ? reachable && localPowerStatus : reachable;
}

// Gültige Knoten-ID: genau 12 Hex-Zeichen (MAC ohne Doppelpunkte, klein)
bool isValidNodeId(const String& id) {
    if (id.length() != 12) return false;
    for (unsigned int i = 0; i < id.length(); i++) {
        char c = id[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
    }
    return true;
}

// Status eines anderen ESP per HTTP abfragen
bool fetchRemoteStatus(const String& ip, RemoteESP& esp) {
    HTTPClient http;
    http.begin("http://" + ip + "/api/localstatus");
    http.setConnectTimeout(1000);
    http.setTimeout(2000);
    int httpCode = http.GET();
    bool ok = false;
    if (httpCode == 200) {
        DynamicJsonDocument doc(1024);
        if (deserializeJson(doc, http.getString()) == DeserializationError::Ok && doc["id"].is<const char*>()) {
            esp.id = sanitizeText(doc["id"].as<String>(), 16);
            esp.hostname = sanitizeText(doc["hostname"] | "", 63);
            esp.ip = ip;
            esp.serverName = sanitizeText(doc["serverName"] | "", 32);
            esp.serverIP = sanitizeText(doc["serverIP"] | "", 45);
            esp.serverPort = doc["serverPort"] | 0;
            esp.hasReset = doc["hasReset"] | false;
            esp.version = sanitizeText(doc["version"] | "", 16);
            esp.serverOnline = doc["serverOnline"] | false;
            esp.serverPower = doc["serverPower"] | false;
            esp.powerSense = doc["powerSense"] | true;
            esp.pingTime = doc["pingTime"] | 0;
            esp.uptime = doc["uptime"] | 0UL;
            esp.rssi = doc["rssi"] | 0;
            ok = isValidNodeId(esp.id);
        }
    }
    http.end();
    return ok;
}

// ESP Netzwerk Scan: per mDNS gefundene und bereits bekannte ESPs abfragen (Issues #14, #15)
void scanForESPs() {
    std::vector<String> candidates;
    int n = MDNS.queryService(mdnsServiceName, "tcp");
    for (int i = 0; i < n; ++i) {
        IPAddress ip = MDNS.IP(i);
        if (ip == WiFi.localIP()) continue;  // uns selbst über die IP erkennen, nicht über den Hostnamen
        candidates.push_back(ip.toString());
    }
    // Bekannte ESPs auch dann abfragen, wenn sie in dieser Runde nicht per mDNS antworten
    xSemaphoreTake(remoteMutex, portMAX_DELAY);
    for (auto& known : remoteESPs) {
        if (std::find(candidates.begin(), candidates.end(), known.second.ip) == candidates.end()) {
            candidates.push_back(known.second.ip);
        }
    }
    xSemaphoreGive(remoteMutex);

    std::vector<String> reached;
    for (const String& ip : candidates) {
        RemoteESP esp;
        bool ok = fetchRemoteStatus(ip, esp);
        esp_task_wdt_reset();
        if (!ok || esp.id == nodeId) continue;
        esp.espReachable = true;
        esp.lastSeen = millis();
        xSemaphoreTake(remoteMutex, portMAX_DELAY);
        remoteESPs[esp.id] = esp;
        xSemaphoreGive(remoteMutex);
        reached.push_back(esp.id);
    }

    // Nicht erreichte ESPs als "nicht erreichbar" markieren, erst nach 24 h vergessen
    xSemaphoreTake(remoteMutex, portMAX_DELAY);
    auto it = remoteESPs.begin();
    while (it != remoteESPs.end()) {
        if (std::find(reached.begin(), reached.end(), it->first) == reached.end()) {
            if (it->second.espReachable) Serial.println("ESP nicht erreichbar: " + it->second.hostname);
            it->second.espReachable = false;
        }
        if (millis() - it->second.lastSeen > remoteForgetTimeout) {
            it = remoteESPs.erase(it);
        } else {
            ++it;
        }
    }
    xSemaphoreGive(remoteMutex);
}

// JSON Status für alle Server erstellen: {"servers":{...}}, mit type zusätzlich {"type":...}
String getStatusJson(const char* type) {
    DynamicJsonDocument doc(8192);
    if (type) doc["type"] = type;
    JsonObject servers = doc.createNestedObject("servers");
    
    // Lokaler Server
    JsonObject local = servers.createNestedObject("local");
    local["isLocal"] = true;
    local["id"] = nodeId;
    local["hostname"] = espHostname;
    local["serverName"] = serverName;
    local["serverIP"] = serverIP;
    local["serverPort"] = serverCheckPort;
    local["espIP"] = WiFi.localIP().toString();
    local["serverOnline"] = localServerStatus;
    if (usePowerSense) local["serverPower"] = localPowerStatus;
    else local["serverPower"] = nullptr;
    local["espReachable"] = true;
    local["pingTime"] = localPingTime;
    local["hasReset"] = RESET_BUTTON_PIN != -1;
    local["version"] = firmwareVersion;
    
    // Remote Server
    xSemaphoreTake(remoteMutex, portMAX_DELAY);
    for (auto& entry : remoteESPs) {
        const RemoteESP& esp = entry.second;
        JsonObject remote = servers.createNestedObject(entry.first);
        remote["isLocal"] = false;
        remote["id"] = esp.id;
        remote["hostname"] = esp.hostname;
        remote["serverName"] = esp.serverName;
        remote["serverIP"] = esp.serverIP;
        remote["serverPort"] = esp.serverPort;
        remote["espIP"] = esp.ip;
        remote["serverOnline"] = esp.serverOnline;
        if (esp.powerSense) remote["serverPower"] = esp.serverPower;
        else remote["serverPower"] = nullptr;
        remote["espReachable"] = esp.espReachable;
        remote["lastSeenAgo"] = millis() - esp.lastSeen;
        remote["pingTime"] = esp.pingTime;
        remote["hasReset"] = esp.hasReset;
        remote["version"] = esp.version;
        remote["uptime"] = esp.uptime;
        remote["rssi"] = esp.rssi;
    }
    xSemaphoreGive(remoteMutex);
    if (doc.overflowed()) Serial.println("Warnung: Status-JSON zu groß, Einträge fehlen");
    
    String output;
    serializeJson(doc, output);
    return output;
}

// WebSocket Event Handler
void webSocketEvent(uint8_t num, WStype_t type, uint8_t * payload, size_t length) {
    switch(type) {
        case WStype_DISCONNECTED:
            Serial.printf("WebSocket Client [%u] getrennt\n", num);
            break;
            
        case WStype_CONNECTED:
            {
                IPAddress ip = webSocket.remoteIP(num);
                Serial.printf("WebSocket Client [%u] verbunden von %s\n", num, ip.toString().c_str());
                
                // Status senden bei Verbindung
                String json = getStatusJson("update");
                webSocket.sendTXT(num, json);
            }
            break;
            
        case WStype_TEXT:
            Serial.printf("WebSocket Text von [%u]: %s\n", num, payload);
            break;
    }
}

// Status an alle WebSocket Clients senden
void sendStatusToClients() {
    String json = getStatusJson("update");
    webSocket.broadcastTXT(json);
}

// Taster nicht-blockierend drücken: Pin HIGH, loop() lässt nach Ablauf wieder los
int pressedPin = -1;
unsigned long pressStart = 0;
unsigned long pressDuration = 0;

bool pressButton(int pin, unsigned long durationMs) {
    if (pressedPin != -1) return false;
    digitalWrite(pin, HIGH);
    pressedPin = pin;
    pressStart = millis();
    pressDuration = durationMs;
    return true;
}

void releaseButtonIfDue() {
    if (pressedPin != -1 && millis() - pressStart >= pressDuration) {
        digitalWrite(pressedPin, LOW);
        pressedPin = -1;
    }
}

// Läuft der Server? Spannung am Mainboard oder Dienst erreichbar
bool serverIsOn() {
    return localPowerStatus || localServerReachable;
}

// Lokalen Server steuern (Issues #11, #12, #13)
ActionResult executeLocalAction(const String& action) {
    if (action != "power" && action != "shutdown" && action != "reset") {
        return {400, "Unbekannte Aktion: " + action};
    }
    if (action == "reset" && RESET_BUTTON_PIN == -1) {
        return {501, "Kein Reset-Pin konfiguriert"};
    }
    bool on = serverIsOn();
    if (action == "power" && on) return {409, "Server läuft bereits"};
    if (action == "shutdown" && !on) return {409, "Server ist bereits aus"};
    if (action == "reset" && !on) return {409, "Server ist aus"};

    int pin = action == "reset" ? RESET_BUTTON_PIN : POWER_BUTTON_PIN;
    unsigned long duration = action == "reset" ? resetButtonTime : powerButtonTime;
    if (!pressButton(pin, duration)) return {409, "Es wird bereits ein Taster gedrückt"};
    Serial.println("Aktion ausgeführt: " + action);
    return {200, ""};
}

// Befehl an einen anderen ESP weiterleiten
ActionResult forwardRemoteAction(const String& target, const String& action) {
    String remoteIP;
    xSemaphoreTake(remoteMutex, portMAX_DELAY);
    auto found = remoteESPs.find(target);
    if (found != remoteESPs.end()) remoteIP = found->second.ip;
    xSemaphoreGive(remoteMutex);
    if (remoteIP.length() == 0) return {404, "Unbekanntes Ziel: " + target};

    DynamicJsonDocument request(256);
    request["action"] = action;
    String payload;
    serializeJson(request, payload);

    HTTPClient http;
    http.begin("http://" + remoteIP + "/api/control");
    http.addHeader("Content-Type", "application/json");
    http.setConnectTimeout(1000);
    http.setTimeout(3000);
    int httpCode = http.POST(payload);
    ActionResult result = {200, ""};
    if (httpCode < 0) {
        result = {504, "ESP nicht erreichbar"};
    } else if (httpCode != 200) {
        DynamicJsonDocument response(256);
        deserializeJson(response, http.getString());
        result = {httpCode, response["error"] | "Fehler beim Ziel-ESP"};
    }
    http.end();
    Serial.println("Remote-Befehl " + action + " an " + target + ": " + String(result.code));
    return result;
}

// JSON-Body eines Requests lesen; bei Fehler wird direkt 400 gesendet
bool readJsonBody(DynamicJsonDocument& doc) {
    if (!server.hasArg("plain") || deserializeJson(doc, server.arg("plain")) != DeserializationError::Ok || !doc.is<JsonObject>()) {
        server.send(400, "application/json", "{\"success\":false,\"error\":\"Ungültiges JSON\"}");
        return false;
    }
    return true;
}

void sendActionResult(const ActionResult& result) {
    DynamicJsonDocument doc(256);
    doc["success"] = result.code == 200;
    if (result.code != 200) doc["error"] = result.error;
    String output;
    serializeJson(doc, output);
    server.send(result.code, "application/json", output);
}

// Web Server Setup
void setupWebServer() {
    // Hauptseite
    server.on("/", []() {
        server.send_P(200, "text/html", htmlTemplate);
    });
    
    // API Endpoints
    server.on("/api/status", []() {
        server.send(200, "application/json", getStatusJson());
    });
    
    server.on("/api/localstatus", []() {
        DynamicJsonDocument doc(768);
        doc["id"] = nodeId;
        doc["hostname"] = espHostname;
        doc["serverName"] = serverName;
        doc["serverIP"] = serverIP;
        doc["serverPort"] = serverCheckPort;
        doc["serverOnline"] = localServerStatus;
        if (usePowerSense) doc["serverPower"] = localPowerStatus;
        else doc["serverPower"] = nullptr;
        doc["powerSense"] = usePowerSense;
        doc["pingTime"] = localPingTime;
        doc["hasReset"] = RESET_BUTTON_PIN != -1;
        doc["version"] = firmwareVersion;
        doc["uptime"] = millis();
        doc["rssi"] = WiFi.RSSI();
        
        String output;
        serializeJson(doc, output);
        server.send(200, "application/json", output);
    });
    
    server.on("/api/control", HTTP_POST, []() {
        DynamicJsonDocument doc(256);
        if (!readJsonBody(doc)) return;
        sendActionResult(executeLocalAction(doc["action"] | ""));
    });
    
    server.on("/control", HTTP_POST, []() {
        DynamicJsonDocument doc(256);
        if (!readJsonBody(doc)) return;
        String target = doc["target"] | "";
        String action = doc["action"] | "";
        if (target.length() == 0) {
            sendActionResult({400, "Kein Ziel angegeben"});
        } else if (target == "local") {
            sendActionResult(executeLocalAction(action));
        } else {
            sendActionResult(forwardRemoteAction(target, action));
        }
    });
    
    server.begin();
    Serial.println("Web Server gestartet auf Port 80");
}

// Hintergrund-Task: lokaler Status-Check und Suche nach anderen ESPs.
// Alles Blockierende (TCP-Connect, mDNS-Query, HTTP-Abfragen) passiert hier, nicht im loop().
void monitorTask(void* param) {
    unsigned long lastScan = 0;
    unsigned long nextScanDelay = 0;  // erster Scan sofort
    esp_task_wdt_add(NULL);
    for (;;) {
        esp_task_wdt_reset();
        updateLocalStatus();
        if (millis() - lastScan >= nextScanDelay) {
            scanForESPs();
            lastScan = millis();
            // Jitter, damit sich die Scan-Phasen mehrerer ESPs nicht dauerhaft überlagern (Issue #3)
            nextScanDelay = scanInterval + random(0, 3000);
        }
        vTaskDelay(pdMS_TO_TICKS(statusCheckInterval));
    }
}

// Watchdog: startet den ESP neu, wenn loop() oder die Monitor-Task hängen (Issue #4)
void setupWatchdog() {
#if ESP_IDF_VERSION_MAJOR >= 5
    esp_task_wdt_config_t config = { .timeout_ms = watchdogTimeoutS * 1000, .idle_core_mask = 0, .trigger_panic = true };
    esp_task_wdt_reconfigure(&config);
#else
    esp_task_wdt_init(watchdogTimeoutS, true);
#endif
    enableLoopWDT();
}

// WebSocket Setup
void setupWebSocket() {
    webSocket.begin();
    webSocket.onEvent(webSocketEvent);
    // Tote Clients (Handy im Standby, eingefrorener Tab) nach ausbleibendem Pong trennen (Issue #17)
    webSocket.enableHeartbeat(15000, 3000, 2);
    Serial.println("WebSocket Server gestartet auf Port 81");
}

// Setup
void setup() {
    Serial.begin(115200);
    delay(1000);
    
    // GPIO Setup
    pinMode(POWER_CHECK_PIN, INPUT_PULLDOWN);
    pinMode(POWER_BUTTON_PIN, OUTPUT);
    digitalWrite(POWER_BUTTON_PIN, LOW);
    
    if (RESET_BUTTON_PIN != -1) {
        pinMode(RESET_BUTTON_PIN, OUTPUT);
        digitalWrite(RESET_BUTTON_PIN, LOW);
    }
    
    // Netzwerk Setup
    setupWiFi();
    setupMDNS();
    setupWebServer();
    setupWebSocket();
    
    // Initialer Status Check, danach übernimmt die Hintergrund-Task
    updateLocalStatus();
    remoteMutex = xSemaphoreCreateMutex();
    setupWatchdog();
    xTaskCreate(monitorTask, "monitor", 8192, NULL, 1, NULL);
    
    Serial.println("\n=== ServerWatch Multi bereit ===");
    Serial.println("Web Interface: http://" + WiFi.localIP().toString());
    Serial.println("WebSocket: ws://" + WiFi.localIP().toString() + ":81");
}

// Main Loop
void loop() {
    server.handleClient();
    webSocket.loop();
    releaseButtonIfDue();
    
    unsigned long currentMillis = millis();
    
    // WiFi Verbindung prüfen und ggf. neu verbinden
    if (currentMillis - lastWiFiCheck >= wifiCheckInterval) {
        lastWiFiCheck = currentMillis;
        if (WiFi.status() != WL_CONNECTED) {
            if (!wifiLost) {
                wifiLost = true;
                wifiLostSince = currentMillis;
            } else if (currentMillis - wifiLostSince >= wifiRestartTimeout) {
                Serial.println("WiFi seit langem getrennt - Neustart");
                ESP.restart();
            }
            Serial.println("WiFi Verbindung verloren! Versuche neu zu verbinden...");
            WiFi.disconnect();
            WiFi.begin(ssid, password);
        } else {
            wifiLost = false;
        }
    }
    
    // Status periodisch an WebSocket-Clients senden (Werte liefert die Hintergrund-Task)
    if (currentMillis - lastStatusBroadcast >= statusCheckInterval) {
        lastStatusBroadcast = currentMillis;
        sendStatusToClients();
    }
}
