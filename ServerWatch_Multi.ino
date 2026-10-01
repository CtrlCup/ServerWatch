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
#include <mbedtls/md.h>

// ========================================
// KONFIGURATIONSVARIABLEN - BITTE ANPASSEN
// ========================================

// Zugangsdaten stehen in secrets.h (Vorlage: secrets.example.h), nicht im Sketch (Issue #20)
#if __has_include("secrets.h")
#include "secrets.h"
#endif
#ifndef WIFI_SSID
#define WIFI_SSID "DEIN_WLAN_NAME"
#endif
#ifndef WIFI_PASSWORD
#define WIFI_PASSWORD "DEIN_WLAN_PASSWORT"
#endif
#ifndef WEB_USER
#define WEB_USER "admin"
#endif
#ifndef WEB_PASSWORD
#define WEB_PASSWORD "serverwatch"                // Standard-Passwort: das Dashboard warnt davor
#endif
#ifndef SWARM_KEY
#define SWARM_KEY ""                              // leer = Schwarm deaktiviert
#endif
const char* ssid = WIFI_SSID;
const char* password = WIFI_PASSWORD;
const char* webUser = WEB_USER;
const char* webPassword = WEB_PASSWORD;
const char* swarmKey = SWARM_KEY;

// Server Konfiguration
const char* serverName = "Heimserver";            // Name deines Servers
const char* serverIP = "192.168.178.1";           // IP-Adresse des zu überwachenden Servers
const int serverCheckPort = 80;                   // Port für Server-Check (80=HTTP, 22=SSH, 445=SMB)

// GPIO Pin Konfiguration
const int POWER_CHECK_PIN = 4;                    // Pin zum Prüfen der Spannung vom Server
const int POWER_BUTTON_PIN = 3;                   // Pin zum Server Ein/Ausschalten
const int RESET_BUTTON_PIN = 5;                   // Pin zum Server Reset (optional, -1 wenn nicht verwendet)
const bool usePowerSense = true;                  // false, wenn POWER_CHECK_PIN nicht mit dem Mainboard verbunden ist
const bool reduceTxPower = false;                 // true senkt die WLAN-Sendeleistung auf 8,5 dBm (hilft z. B. beim ESP32-C3 Super Mini mit schwacher Antenne, Issue #23)

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

const char* firmwareVersion = "1.0.12";

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
// Diagnose (Issue #23): Zähler werden im WLAN-Event-Task geschrieben, daher volatile
volatile uint32_t wifiDisconnects = 0;
volatile uint8_t lastDisconnectReason = 0;
unsigned long maxLoopMs = 0;
volatile bool localServerStatus = false;
volatile bool localServerReachable = false;
volatile bool localPowerStatus = false;
volatile int localPingTime = 0;
String espHostname;
String nodeId;                                    // eindeutige ID dieses ESP (MAC ohne Doppelpunkte)
String wsToken;                                   // Zugangstoken für den WebSocket, neu bei jedem Boot
bool wsAuthed[WEBSOCKETS_SERVER_CLIENT_MAX] = {false};
bool swarmEnabled = false;                        // nur mit eigenem SWARM_KEY (Issue #10)
bool defaultCredentials = false;
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

// WLAN-Ereignisse mitschreiben (Issue #23). Läuft im Event-Task: nichts Blockierendes, keine WiFi-Aufrufe
void onWiFiEvent(arduino_event_id_t event, arduino_event_info_t info) {
    if (event == ARDUINO_EVENT_WIFI_STA_DISCONNECTED) {
        uint8_t reason = info.wifi_sta_disconnected.reason;
        Serial.printf("WiFi getrennt, Grund %u (%s)\n", reason, WiFi.disconnectReasonName((wifi_err_reason_t)reason));
        // Grund 8 (ASSOC_LEAVE) löst unser eigenes WiFi.disconnect() im Reconnect-Fallback aus: nicht zählen
        if (reason != WIFI_REASON_ASSOC_LEAVE) {
            wifiDisconnects++;
            lastDisconnectReason = reason;
        }
    } else if (event == ARDUINO_EVENT_WIFI_STA_GOT_IP) {
        Serial.println("WiFi: IP erhalten");
    } else if (event == ARDUINO_EVENT_WIFI_STA_LOST_IP) {
        Serial.println("WiFi: IP verloren");
    }
}

// WLAN starten; mit reduceTxPower danach die Sendeleistung senken (Issue #23)
void startWiFi() {
    WiFi.begin(ssid, password);
    if (reduceTxPower) WiFi.setTxPower(WIFI_POWER_8_5dBm);
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
    
    WiFi.onEvent(onWiFiEvent);  // vor WiFi.begin() registrieren, damit kein Ereignis fehlt (Issue #23)
    startWiFi();
    if (reduceTxPower) Serial.println("Sendeleistung reduziert (8,5 dBm)");
    
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
    
    if (!swarmEnabled) {
        Serial.println("Schwarm deaktiviert: SWARM_KEY (mind. 16 Zeichen) in secrets.h setzen");
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
    // Challenge-Response: die Antwort muss mit dem Schwarm-Schlüssel signiert sein (Issue #10)
    String challenge = randomHex(16);
    HTTPClient http;
    http.begin("http://" + ip + "/api/localstatus?c=" + challenge);
    http.setConnectTimeout(1000);
    http.setTimeout(2000);
    int httpCode = http.GET();
    bool ok = false;
    String payload;
    if (httpCode == 200) {
        DynamicJsonDocument envelope(1536);
        if (deserializeJson(envelope, http.getString()) == DeserializationError::Ok) {
            String signedPayload = envelope["payload"] | "";
            String sig = envelope["sig"] | "";
            if (signedPayload.length() > 0 && constantTimeEquals(sig, hmacHex("st|" + challenge + "|" + signedPayload))) {
                payload = signedPayload;
            }
        }
    }
    http.end();
    if (payload.length() > 0) {
        DynamicJsonDocument doc(1024);
        // Die signierte Antwort muss die angefragte IP nennen, sonst leitet jemand nur weiter (Relay)
        if (deserializeJson(doc, payload) == DeserializationError::Ok && doc["id"].is<const char*>() && ip == (doc["ip"] | "")) {
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
    doc["defaultCredentials"] = defaultCredentials;
    doc["swarmEnabled"] = swarmEnabled;
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

// Wert des Query-Parameters t aus einer URL wie "/?t=abc" (exakter Parametername)
String wsTokenFromUrl(const String& url) {
    int query = url.indexOf('?');
    if (query < 0) return "";
    String rest = url.substring(query + 1);
    while (rest.length() > 0) {
        int amp = rest.indexOf('&');
        String param = amp < 0 ? rest : rest.substring(0, amp);
        if (param.startsWith("t=")) return param.substring(2);
        rest = amp < 0 ? "" : rest.substring(amp + 1);
    }
    return "";
}

// WebSocket Event Handler
void webSocketEvent(uint8_t num, WStype_t type, uint8_t * payload, size_t length) {
    switch(type) {
        case WStype_DISCONNECTED:
            wsAuthed[num] = false;
            Serial.printf("WebSocket Client [%u] getrennt\n", num);
            break;
            
        case WStype_CONNECTED:
            {
                IPAddress ip = webSocket.remoteIP(num);
                // Token aus der URL (?t=...) prüfen, bevor irgendetwas gesendet wird (Issue #8)
                if (!constantTimeEquals(wsTokenFromUrl(String((const char*)payload).substring(0, length)), wsToken)) {
                    Serial.printf("WebSocket Client [%u] ohne gültigen Token abgewiesen\n", num);
                    webSocket.disconnect(num);
                    break;
                }
                wsAuthed[num] = true;
                Serial.printf("WebSocket Client [%u] verbunden von %s\n", num, ip.toString().c_str());
                
                // Status senden bei Verbindung
                String json = getStatusJson("update");
                webSocket.sendTXT(num, json);
            }
            break;
            
        case WStype_TEXT:
            break;  // der Server erwartet keine Nachrichten von Clients
    }
}

// Status an alle WebSocket Clients senden
void sendStatusToClients() {
    String json = getStatusJson("update");
    for (uint8_t num = 0; num < WEBSOCKETS_SERVER_CLIENT_MAX; num++) {
        if (wsAuthed[num]) webSocket.sendTXT(num, json);  // nur an Clients mit gültigem Token
    }
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

    // Einmal-Nonce beim Ziel holen und den Befehl an Ziel-ID, Nonce und Aktion gebunden signieren
    String nonce;
    {
        HTTPClient http;
        http.begin("http://" + remoteIP + "/api/nonce");
        http.setConnectTimeout(1000);
        http.setTimeout(2000);
        if (http.GET() == 200) {
            DynamicJsonDocument doc(256);
            if (deserializeJson(doc, http.getString()) == DeserializationError::Ok) nonce = doc["nonce"] | "";
        }
        http.end();
    }
    if (!isLowerHex(nonce, 32)) return {504, "ESP nicht erreichbar"};

    DynamicJsonDocument request(512);
    request["action"] = action;
    request["nonce"] = nonce;
    request["sig"] = hmacHex("ctl|" + target + "|" + nonce + "|" + action);
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
    if (!server.hasArg("plain") || deserializeJson(doc, server.arg("plain")) != DeserializationError::Ok || !doc.is<JsonObject>() ||
        (doc.containsKey("action") && !doc["action"].is<const char*>()) || (doc.containsKey("target") && !doc["target"].is<const char*>())) {
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

// ================= Sicherheit (Issues #8, #9, #10, #18) =================

// Zufällige Hex-Zeichenkette aus dem Hardware-Zufallsgenerator
String randomHex(int bytes) {
    String out;
    char buf[3];
    for (int i = 0; i < bytes; i++) {
        snprintf(buf, sizeof buf, "%02x", (unsigned int)(esp_random() & 0xff));
        out += buf;
    }
    return out;
}

// Genau len Zeichen, nur 0-9 und a-f
bool isLowerHex(const String& text, unsigned int len) {
    if (text.length() != len) return false;
    for (unsigned int i = 0; i < len; i++) {
        char c = text[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
    }
    return true;
}

bool constantTimeEquals(const String& a, const String& b) {
    if (a.length() != b.length()) return false;
    unsigned char diff = 0;
    for (unsigned int i = 0; i < a.length(); i++) diff |= a[i] ^ b[i];
    return diff == 0;
}

// HMAC-SHA256 mit dem Schwarm-Schlüssel, als Hex
String hmacHex(const String& message) {
    unsigned char mac[32];
    mbedtls_md_hmac(mbedtls_md_info_from_type(MBEDTLS_MD_SHA256), (const unsigned char*)swarmKey, strlen(swarmKey),
                    (const unsigned char*)message.c_str(), message.length(), mac);
    String out;
    char buf[3];
    for (int i = 0; i < 32; i++) {
        snprintf(buf, sizeof buf, "%02x", mac[i]);
        out += buf;
    }
    return out;
}

// Ist host (ggf. mit Port) eine Adresse dieses ESP? Schutz gegen DNS-Rebinding.
bool isOwnHost(String host) {
    host.toLowerCase();
    int colon = host.indexOf(':');
    if (colon >= 0) host = host.substring(0, colon);
    if (host.length() == 0) return false;
    if (host == WiFi.localIP().toString()) return true;
    return host == espHostname || host.startsWith(espHostname + ".");  // z. B. .local oder .fritz.box
}

// Origin leer (kein Browser, andere ESPs) oder eigene Adresse
bool isAllowedOrigin(const String& origin) {
    if (origin.length() == 0) return true;
    if (!origin.startsWith("http://")) return false;  // auch "null"
    String host = origin.substring(7);
    if (host.indexOf('/') >= 0) return false;
    return isOwnHost(host);
}

void sendError(int code, const char* message) {
    DynamicJsonDocument doc(256);
    doc["success"] = false;
    doc["error"] = message;
    String output;
    serializeJson(doc, output);
    server.send(code, "application/json", output);
}

// Jede Anfrage: Host-Header muss zu diesem ESP gehören
bool checkHost() {
    if (isOwnHost(server.hostHeader())) return true;
    sendError(403, "Unbekannter Host");
    return false;
}

// Bremse gegen Durchprobieren von Passwörtern: 1 s Sperre pro IP nach Fehlversuch
struct FailedLogin {
    uint32_t ip;
    unsigned long at;
};
FailedLogin failedLogins[8] = {};
int failedLoginNext = 0;

bool loginThrottled(uint32_t ip) {
    for (const FailedLogin& f : failedLogins) {
        if (f.ip == ip && f.at != 0 && millis() - f.at < 1000) return true;
    }
    return false;
}

// Host prüfen und Login verlangen (HTTP Basic Auth)
bool requireAuth() {
    if (!checkHost()) return false;
    uint32_t ip = (uint32_t)server.client().remoteIP();
    if (loginThrottled(ip)) {
        sendError(429, "Zu viele Fehlversuche, bitte kurz warten");
        return false;
    }
    if (server.authenticate(webUser, webPassword)) return true;
    if (server.hasHeader("Authorization")) {
        failedLogins[failedLoginNext] = {ip, millis() | 1};
        failedLoginNext = (failedLoginNext + 1) % 8;
    }
    server.requestAuthentication(BASIC_AUTH, "ServerWatch");
    return false;
}

// Schreibende Anfragen: nur JSON und nur von der eigenen Seite (CSRF)
bool checkJsonWrite() {
    String contentType = server.header("Content-Type");
    contentType.toLowerCase();
    if (!contentType.startsWith("application/json")) {
        sendError(415, "Content-Type application/json erforderlich");
        return false;
    }
    if (!isAllowedOrigin(server.header("Origin"))) {
        sendError(403, "Fremder Origin");
        return false;
    }
    return true;
}

// Einmal-Nonces für Befehle anderer ESPs (30 s gültig)
struct NonceEntry {
    String value;
    unsigned long issued;
};
const int nonceSlots = 16;
NonceEntry nonces[nonceSlots];
int nonceNext = 0;

String issueNonce() {
    String nonce = randomHex(16);
    nonces[nonceNext] = {nonce, millis()};
    nonceNext = (nonceNext + 1) % nonceSlots;
    return nonce;
}

// Nonce prüfen und in jedem Fall verbrauchen
bool consumeNonce(const String& nonce) {
    if (!isLowerHex(nonce, 32)) return false;
    for (NonceEntry& entry : nonces) {
        if (entry.value.length() > 0 && constantTimeEquals(entry.value, nonce)) {
            bool fresh = millis() - entry.issued < 30000;
            entry.value = "";
            return fresh;
        }
    }
    return false;
}

// ================= Ende Sicherheit =================

// Web Server Setup
void setupWebServer() {
    // Hauptseite
    server.on("/", []() {
        if (!requireAuth()) return;
        server.send_P(200, "text/html", htmlTemplate);
    });
    
    // API Endpoints für das Dashboard (Login erforderlich)
    server.on("/api/status", []() {
        if (!requireAuth()) return;
        server.send(200, "application/json", getStatusJson());
    });
    
    server.on("/api/wstoken", []() {
        if (!requireAuth()) return;
        server.sendHeader("Cache-Control", "no-store");
        server.send(200, "application/json", "{\"token\":\"" + wsToken + "\"}");
    });
    
    // Diagnose für Feldfehler wie "ESP nicht mehr erreichbar" (Issue #23)
    server.on("/api/diag", HTTP_GET, []() {
        if (!requireAuth()) return;
        DynamicJsonDocument doc(512);
        doc["uptime_ms"] = millis();
        doc["reset_reason"] = (int)esp_reset_reason();
        doc["free_heap"] = ESP.getFreeHeap();
        doc["min_free_heap"] = ESP.getMinFreeHeap();
        doc["rssi"] = WiFi.RSSI();
        doc["wifi_disconnects"] = wifiDisconnects;
        doc["last_disconnect_reason"] = lastDisconnectReason;
        doc["max_loop_ms"] = maxLoopMs;
        doc["version"] = firmwareVersion;
        String json;
        serializeJson(doc, json);
        server.sendHeader("Cache-Control", "no-store");
        server.send(200, "application/json", json);
    });
    
    // API Endpoints für andere ESPs (Schwarm-Schlüssel statt Login)
    server.on("/api/nonce", []() {
        if (!checkHost()) return;
        if (!swarmEnabled) return sendError(403, "Schwarm deaktiviert");
        server.send(200, "application/json", "{\"nonce\":\"" + issueNonce() + "\"}");
    });
    
    server.on("/api/localstatus", []() {
        if (!checkHost()) return;
        if (!swarmEnabled) return sendError(403, "Schwarm deaktiviert");
        String challenge = server.arg("c");
        if (!isLowerHex(challenge, 32)) return sendError(400, "Ungültige Challenge");
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
        doc["ip"] = WiFi.localIP().toString();
        
        String payload;
        serializeJson(doc, payload);
        DynamicJsonDocument envelope(1536);
        envelope["payload"] = payload;
        envelope["sig"] = hmacHex("st|" + challenge + "|" + payload);
        String output;
        serializeJson(envelope, output);
        server.send(200, "application/json", output);
    });
    
    server.on("/api/control", HTTP_POST, []() {
        if (!checkHost()) return;
        if (!swarmEnabled) return sendError(403, "Schwarm deaktiviert");
        if (!checkJsonWrite()) return;
        DynamicJsonDocument doc(512);
        if (!readJsonBody(doc)) return;
        String action = doc["action"] | "";
        String nonce = doc["nonce"] | "";
        String sig = doc["sig"] | "";
        // Nonce wird auch bei falscher Signatur verbraucht; die Signatur bindet Ziel-ID und Aktion
        bool nonceOk = consumeNonce(nonce);
        if (!nonceOk || !constantTimeEquals(sig, hmacHex("ctl|" + nodeId + "|" + nonce + "|" + action))) {
            return sendError(403, "Ungültige Signatur");
        }
        sendActionResult(executeLocalAction(action));
    });
    
    server.on("/control", HTTP_POST, []() {
        if (!requireAuth() || !checkJsonWrite()) return;
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
    
    const char* headerKeys[] = {"Content-Type", "Origin"};
    server.collectHeaders(headerKeys, 2);
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
        if (swarmEnabled && millis() - lastScan >= nextScanDelay) {
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
    // Cross-Site WebSocket Hijacking verhindern: nur eigener oder kein Origin (Issue #18)
    webSocket.onValidateHttpHeader([](String headerName, String headerValue) {
        return !headerName.equalsIgnoreCase("Origin") || isAllowedOrigin(headerValue);
    }, nullptr, 0);
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
    
    // Sicherheit
    wsToken = randomHex(16);
    swarmEnabled = strlen(swarmKey) >= 16;
    defaultCredentials = strcmp(webPassword, "serverwatch") == 0;
    if (defaultCredentials) Serial.println("WARNUNG: Standard-Passwort aktiv - WEB_PASSWORD in secrets.h setzen");
    
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
    unsigned long loopStart = millis();  // Dauer dieses Durchlaufs für /api/diag (Issue #23)
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
            startWiFi();
        } else {
            wifiLost = false;
        }
    }
    
    // Status periodisch an WebSocket-Clients senden (Werte liefert die Hintergrund-Task)
    if (currentMillis - lastStatusBroadcast >= statusCheckInterval) {
        lastStatusBroadcast = currentMillis;
        sendStatusToClients();
    }
    
    unsigned long loopMs = millis() - loopStart;
    if (loopMs > maxLoopMs) maxLoopMs = loopMs;
}
