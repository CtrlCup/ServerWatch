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

const char* firmwareVersion = "1.0.6";

// Webserver und WebSocket
WebServer server(80);
WebSocketsServer webSocket = WebSocketsServer(81);

// Remote ESP Struktur
struct RemoteESP {
    String hostname;
    String ip;
    String serverName;
    bool serverOnline;
    bool serverPower;
    bool espReachable;
    unsigned long lastSeen;
    int pingTime;
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
volatile bool localPowerStatus = false;
volatile int localPingTime = 0;
String espHostname;

// HTML Template
const char htmlTemplate[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="de">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1.0, maximum-scale=1.0, user-scalable=no">
<title>ServerWatch Multi</title>
<link rel="icon" type="image/svg+xml" href="data:image/svg+xml,%3Csvg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 24 24' fill='none' stroke='%234f46e5' stroke-width='2'%3E%3Crect x='2' y='2' width='20' height='8' rx='2'/%3E%3Crect x='2' y='14' width='20' height='8' rx='2'/%3E%3Ccircle cx='6' cy='6' r='1' fill='%234f46e5'/%3E%3Ccircle cx='6' cy='18' r='1' fill='%234f46e5'/%3E%3C/svg%3E">
<style>
*{margin:0;padding:0;box-sizing:border-box}
:root{--bg-primary:#0f0f0f;--bg-secondary:#1a1a1a;--bg-card:linear-gradient(145deg,#1f1f1f,#151515);--border:#2a2a2a;--text-primary:#fff;--text-secondary:#a0a0a0;--accent:#4f46e5;--accent-hover:#6366f1;--success:#22c55e;--warning:#f59e0b;--danger:#ef4444;--shadow:0 4px 20px rgba(0,0,0,0.5)}
body.light{--bg-primary:#f5f5f5;--bg-secondary:#fff;--bg-card:linear-gradient(145deg,#fff,#f0f0f0);--border:#e0e0e0;--text-primary:#1a1a1a;--text-secondary:#666;--shadow:0 4px 20px rgba(0,0,0,0.1)}
body{background:var(--bg-primary);color:var(--text-primary);font-family:-apple-system,BlinkMacSystemFont,'Segoe UI',Roboto,sans-serif;min-height:100vh;transition:background .3s,color .3s;overflow-x:hidden}
.header{background:var(--bg-secondary);padding:1rem 1.5rem;border-bottom:1px solid var(--border);position:sticky;top:0;z-index:100;backdrop-filter:blur(10px)}
.header-content{max-width:1400px;margin:0 auto;display:flex;justify-content:space-between;align-items:center;flex-wrap:wrap;gap:1rem}
.logo{display:flex;align-items:center;gap:.75rem;font-size:1.25rem;font-weight:600}
.logo svg{width:28px;height:28px}
.theme-toggle{background:var(--bg-card);border:1px solid var(--border);border-radius:50px;padding:.5rem;cursor:pointer;transition:all .3s;display:flex;align-items:center;justify-content:center}
.theme-toggle:hover{transform:scale(1.1)}
.theme-toggle svg{width:20px;height:20px;stroke:var(--text-primary)}
.container{max-width:1400px;margin:0 auto;padding:2rem 1rem}
.grid{display:grid;grid-template-columns:repeat(auto-fill,minmax(320px,1fr));gap:1.5rem;margin-bottom:2rem}
.card{background:var(--bg-card);border:1px solid var(--border);border-radius:16px;padding:1.5rem;box-shadow:var(--shadow);transition:transform .2s,box-shadow .2s;position:relative;overflow:hidden}
.card:hover{transform:translateY(-2px);box-shadow:0 6px 25px rgba(0,0,0,0.6)}
.card::before{content:'';position:absolute;top:0;left:0;right:0;height:4px;background:var(--accent);opacity:0;transition:opacity .3s}
.card.local::before{opacity:1}
.card-header{display:flex;justify-content:space-between;align-items:flex-start;margin-bottom:1.25rem}
.card-title{font-size:1.125rem;font-weight:600;display:flex;align-items:center;gap:.5rem}
.status-indicator{width:10px;height:10px;border-radius:50%;animation:pulse 2s infinite}
.status-indicator.online{background:var(--success)}
.status-indicator.offline{background:var(--danger)}
.status-indicator.warning{background:var(--warning)}
@keyframes pulse{0%,100%{opacity:1}50%{opacity:.5}}
.badge{padding:.25rem .75rem;border-radius:20px;font-size:.75rem;font-weight:600;text-transform:uppercase;letter-spacing:.5px}
.badge.local{background:var(--accent);color:#fff}
.badge.remote{background:var(--border);color:var(--text-secondary)}
.info-grid{display:grid;gap:.75rem;margin-bottom:1.25rem}
.info-row{display:flex;justify-content:space-between;padding:.5rem;background:var(--bg-primary);border-radius:8px;font-size:.875rem}
.info-label{color:var(--text-secondary)}
.info-value{font-weight:500}
.button-group{display:flex;gap:.75rem;flex-wrap:wrap}
.btn{flex:1;min-width:100px;padding:.625rem 1rem;border:none;border-radius:8px;font-size:.875rem;font-weight:600;cursor:pointer;transition:all .2s;display:flex;align-items:center;justify-content:center;gap:.5rem}
.btn:disabled{opacity:.5;cursor:not-allowed}
.btn-primary{background:var(--accent);color:#fff}
.btn-primary:hover:not(:disabled){background:var(--accent-hover);transform:translateY(-1px)}
.btn-danger{background:var(--danger);color:#fff}
.btn-danger:hover:not(:disabled){background:#dc2626;transform:translateY(-1px)}
.btn-secondary{background:var(--bg-secondary);color:var(--text-primary);border:1px solid var(--border)}
.btn-secondary:hover:not(:disabled){background:var(--border)}
.btn svg{width:16px;height:16px}
.stats-bar{background:var(--bg-secondary);border-radius:12px;padding:1rem;margin-top:2rem;display:flex;justify-content:space-around;flex-wrap:wrap;gap:1rem}
.stat-item{text-align:center}
.stat-value{font-size:1.5rem;font-weight:700;color:var(--accent)}
.stat-label{font-size:.75rem;color:var(--text-secondary);text-transform:uppercase;letter-spacing:1px;margin-top:.25rem}
@media(max-width:640px){.grid{grid-template-columns:1fr}.header-content{flex-direction:column;text-align:center}.button-group{flex-direction:column}.btn{width:100%}}
.notification{position:fixed;bottom:2rem;right:2rem;background:var(--bg-secondary);border:1px solid var(--border);border-radius:12px;padding:1rem 1.5rem;box-shadow:var(--shadow);transform:translateX(400px);transition:transform .3s;z-index:1000;max-width:90vw}
.notification.show{transform:translateX(0)}
.notification.success{border-left:4px solid var(--success)}
.notification.error{border-left:4px solid var(--danger)}
@media(max-width:640px){.notification{right:1rem;left:1rem;bottom:1rem}}
html{scroll-behavior:smooth}
*{-webkit-tap-highlight-color:transparent}
</style>
</head>
<body class="dark">
<div class="header">
<div class="header-content">
<div class="logo">
<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><rect x="2" y="2" width="20" height="8" rx="2"/><rect x="2" y="14" width="20" height="8" rx="2"/><circle cx="6" cy="6" r="1" fill="currentColor"/><circle cx="6" cy="18" r="1" fill="currentColor"/><path d="M10 6h10M10 18h10"/></svg>
<span>ServerWatch Multi</span>
</div>
<button class="theme-toggle" onclick="toggleTheme()">
<svg class="sun" style="display:none" xmlns="http://www.w3.org/2000/svg" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><circle cx="12" cy="12" r="5"/><line x1="12" y1="1" x2="12" y2="3"/><line x1="12" y1="21" x2="12" y2="23"/><line x1="4.22" y1="4.22" x2="5.64" y2="5.64"/><line x1="18.36" y1="18.36" x2="19.78" y2="19.78"/><line x1="1" y1="12" x2="3" y2="12"/><line x1="21" y1="12" x2="23" y2="12"/><line x1="4.22" y1="19.78" x2="5.64" y2="18.36"/><line x1="18.36" y1="5.64" x2="19.78" y2="4.22"/></svg>
<svg class="moon" xmlns="http://www.w3.org/2000/svg" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><path d="M21 12.79A9 9 0 1 1 11.21 3 7 7 0 0 0 21 12.79z"/></svg>
</button>
</div>
</div>
<div class="container">
<div class="grid" id="serverGrid"></div>
<div class="stats-bar">
<div class="stat-item"><div class="stat-value" id="totalServers">0</div><div class="stat-label">Server Total</div></div>
<div class="stat-item"><div class="stat-value" id="onlineServers">0</div><div class="stat-label">Online</div></div>
<div class="stat-item"><div class="stat-value" id="offlineServers">0</div><div class="stat-label">Offline</div></div>
<div class="stat-item"><div class="stat-value" id="connectedESPs">0</div><div class="stat-label">ESPs Verbunden</div></div>
</div>
</div>
<div class="notification" id="notification"></div>
<script>
let ws,servers={},darkMode=!0;
const icons={power:'<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><path d="M18.36 6.64a9 9 0 1 1-12.73 0"/><line x1="12" y1="2" x2="12" y2="12"/></svg>',reset:'<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><polyline points="23 4 23 10 17 10"/><path d="M20.49 15a9 9 0 1 1-2.12-9.36L23 10"/></svg>',shutdown:'<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><circle cx="12" cy="12" r="10"/><line x1="8" y1="15" x2="16" y2="15"/><line x1="9" y1="9" x2="9.01" y2="9"/><line x1="15" y1="9" x2="15.01" y2="9"/></svg>'};
function toggleTheme(){darkMode=!darkMode,document.body.classList.toggle("light"),document.querySelector(".sun").style.display=darkMode?"none":"block",document.querySelector(".moon").style.display=darkMode?"block":"none",localStorage.setItem("darkMode",darkMode)}
function initTheme(){const e=localStorage.getItem("darkMode");null!==e&&(darkMode="true"===e),darkMode||(document.body.classList.add("light"),document.querySelector(".sun").style.display="block",document.querySelector(".moon").style.display="none")}
function connectWebSocket(){ws=new WebSocket(`ws://${window.location.hostname}:81`),ws.onopen=()=>{console.log("WebSocket connected"),showNotification("Verbindung hergestellt","success")},ws.onmessage=e=>{const t=JSON.parse(e.data);"update"===t.type&&updateServers(t.servers)},ws.onclose=()=>{console.log("WebSocket disconnected"),showNotification("Verbindung verloren, versuche neu zu verbinden...","error"),setTimeout(connectWebSocket,5e3)},ws.onerror=e=>{console.error("WebSocket error:",e)}}
function updateServers(e){servers=e,renderServers(),updateStats()}
function renderServers(){const e=document.getElementById("serverGrid");e.innerHTML="",Object.keys(servers).forEach(t=>{const n=servers[t],o=createServerCard(t,n);e.appendChild(o)})}
function createServerCard(e,t){const n=t.isLocal||!1,o=t.serverOnline||!1,s=t.serverPower||!1,r=document.createElement("div");return r.className=`card ${n?"local":""}`,r.innerHTML=`<div class="card-header"><div class="card-title"><span class="status-indicator ${o?"online":s?"warning":"offline"}"></span><span>${t.serverName||"Unbekannt"}</span></div><span class="badge ${n?"local":"remote"}">${n?"Lokal":"Remote"}</span></div><div class="info-grid"><div class="info-row"><span class="info-label">Server IP</span><span class="info-value">${t.serverIP||"-"}</span></div><div class="info-row"><span class="info-label">ESP IP</span><span class="info-value">${t.espIP||"-"}</span></div><div class="info-row"><span class="info-label">Status</span><span class="info-value">${o?"Online":s?"Strom vorhanden":"Offline"}</span></div><div class="info-row"><span class="info-label">Ping</span><span class="info-value">${t.pingTime?t.pingTime+" ms":"-"}</span></div><div class="info-row"><span class="info-label">ESP Status</span><span class="info-value">${!1!==t.espReachable?"Erreichbar":"Nicht erreichbar"}</span></div></div><div class="button-group"><button class="btn btn-primary" onclick="powerAction('${e}','power')" ${o?"disabled":""}>${icons.power}<span>Starten</span></button><button class="btn btn-secondary" onclick="powerAction('${e}','reset')" ${o?"":"disabled"}>${icons.reset}<span>Reset</span></button><button class="btn btn-danger" onclick="powerAction('${e}','shutdown')" ${o?"":"disabled"}>${icons.shutdown}<span>Herunterfahren</span></button></div>`,r}
function powerAction(e,t){servers[e]&&fetch("/control",{method:"POST",headers:{"Content-Type":"application/json"},body:JSON.stringify({target:e,action:t})}).then(e=>e.json()).then(e=>{e.success?showNotification(t+" wurde ausgeführt","success"):showNotification("Fehler: "+e.error,"error")}).catch(e=>{showNotification("Verbindungsfehler: "+e,"error")})}
function updateStats(){let e=0,t=0,n=0,o=0;Object.values(servers).forEach(s=>{e++,s.serverOnline?t++:n++,!1!==s.espReachable&&o++}),document.getElementById("totalServers").textContent=e,document.getElementById("onlineServers").textContent=t,document.getElementById("offlineServers").textContent=n,document.getElementById("connectedESPs").textContent=o}
function showNotification(e,t="info"){const n=document.getElementById("notification");n.className=`notification ${t}`,n.textContent=e,n.classList.add("show"),setTimeout(()=>{n.classList.remove("show")},3e3)}
function fetchStatus(){fetch("/api/status").then(e=>e.json()).then(e=>{updateServers(e.servers||{})}).catch(e=>{console.error("Error fetching status:",e)})}
initTheme(),connectWebSocket(),fetchStatus(),setInterval(fetchStatus,5e3);
</script>
</body>
</html>
)rawliteral";

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
void handleRemoteCommand(String target, String action);
String getStatusJson(const char* type = nullptr);

// WiFi Setup
void setupWiFi() {
    Serial.println("\n=== ServerWatch Multi ESP32 ===");
    Serial.print("Verbinde mit WiFi: ");
    Serial.println(ssid);
    
    // Hostname generieren
    espHostname = "ServerWatch-" + String(serverName);
    espHostname.replace(" ", "_");
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
    localPowerStatus = (digitalRead(POWER_CHECK_PIN) == HIGH);
    localServerStatus = reachable && localPowerStatus;
}

// ESP Netzwerk Scan
void scanForESPs() {
    Serial.println("Scanne nach ServerWatch ESPs...");
    
    int n = MDNS.queryService(mdnsServiceName, "tcp");
    
    if (n == 0) {
        Serial.println("Keine anderen ServerWatch ESPs gefunden");
        return;
    }
    
    Serial.printf("%d ServerWatch ESP(s) gefunden:\n", n);
    
    for (int i = 0; i < n; ++i) {
        String hostname = MDNS.hostname(i);
        IPAddress ip = MDNS.IP(i);
        
        // Nicht uns selbst hinzufügen
        if (hostname == espHostname) continue;
        
        Serial.printf("  - %s (%s)\n", hostname.c_str(), ip.toString().c_str());
        
        // Remote ESP Status abrufen
        HTTPClient http;
        http.begin("http://" + ip.toString() + "/api/localstatus");
        http.setConnectTimeout(1000);
        http.setTimeout(2000);
        
        int httpCode = http.GET();
        
        esp_task_wdt_reset();
        if (httpCode == 200) {
            String payload = http.getString();
            DynamicJsonDocument doc(512);
            
            if (deserializeJson(doc, payload) == DeserializationError::Ok) {
                RemoteESP esp;
                esp.hostname = hostname;
                esp.ip = ip.toString();
                esp.serverName = doc["serverName"].as<String>();
                esp.serverOnline = doc["serverOnline"];
                esp.serverPower = doc["serverPower"];
                esp.pingTime = doc["pingTime"];
                esp.espReachable = true;
                esp.lastSeen = millis();
                
                xSemaphoreTake(remoteMutex, portMAX_DELAY);
                remoteESPs[hostname] = esp;
                xSemaphoreGive(remoteMutex);
            }
        } else {
            // ESP nicht erreichbar markieren
            xSemaphoreTake(remoteMutex, portMAX_DELAY);
            if (remoteESPs.find(hostname) != remoteESPs.end()) {
                remoteESPs[hostname].espReachable = false;
            }
            xSemaphoreGive(remoteMutex);
        }
        
        http.end();
    }
    
    // Alte ESPs entfernen (nicht mehr im Netzwerk)
    xSemaphoreTake(remoteMutex, portMAX_DELAY);
    auto it = remoteESPs.begin();
    while (it != remoteESPs.end()) {
        if (millis() - it->second.lastSeen > 60000) { // 60 Sekunden Timeout
            Serial.println("Entferne inaktiven ESP: " + it->first);
            it = remoteESPs.erase(it);
        } else {
            ++it;
        }
    }
    xSemaphoreGive(remoteMutex);
}

// JSON Status für alle Server erstellen: {"servers":{...}}, mit type zusätzlich {"type":...}
String getStatusJson(const char* type) {
    DynamicJsonDocument doc(4096);
    if (type) doc["type"] = type;
    JsonObject servers = doc.createNestedObject("servers");
    
    // Lokaler Server
    JsonObject local = servers.createNestedObject("local");
    local["isLocal"] = true;
    local["serverName"] = serverName;
    local["serverIP"] = serverIP;
    local["espIP"] = WiFi.localIP().toString();
    local["serverOnline"] = localServerStatus;
    local["serverPower"] = localPowerStatus;
    local["espReachable"] = true;
    local["pingTime"] = localPingTime;
    
    // Remote Server
    xSemaphoreTake(remoteMutex, portMAX_DELAY);
    for (auto& esp : remoteESPs) {
        JsonObject remote = servers.createNestedObject(esp.first);
        remote["isLocal"] = false;
        remote["serverName"] = esp.second.serverName;
        remote["serverIP"] = "Remote";
        remote["espIP"] = esp.second.ip;
        remote["serverOnline"] = esp.second.serverOnline;
        remote["serverPower"] = esp.second.serverPower;
        remote["espReachable"] = esp.second.espReachable;
        remote["pingTime"] = esp.second.pingTime;
    }
    xSemaphoreGive(remoteMutex);
    
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

// Remote Command Handler
void handleRemoteCommand(String target, String action) {
    if (target == "local") {
        // Lokalen Server steuern
        if (action == "power") {
            digitalWrite(POWER_BUTTON_PIN, HIGH);
            delay(powerButtonTime);
            digitalWrite(POWER_BUTTON_PIN, LOW);
            Serial.println("Power Button gedrückt");
        }
        else if (action == "reset" && RESET_BUTTON_PIN != -1) {
            digitalWrite(RESET_BUTTON_PIN, HIGH);
            delay(resetButtonTime);
            digitalWrite(RESET_BUTTON_PIN, LOW);
            Serial.println("Reset Button gedrückt");
        }
    } else {
        // Remote ESP steuern
        String remoteIP;
        xSemaphoreTake(remoteMutex, portMAX_DELAY);
        auto found = remoteESPs.find(target);
        if (found != remoteESPs.end()) remoteIP = found->second.ip;
        xSemaphoreGive(remoteMutex);
        if (remoteIP.length() > 0) {
            HTTPClient http;
            http.begin("http://" + remoteIP + "/api/control");
            http.addHeader("Content-Type", "application/json");
            http.setConnectTimeout(1000);
            http.setTimeout(3000);
            
            String payload = "{\"action\":\"" + action + "\"}";
            int httpCode = http.POST(payload);
            
            if (httpCode == 200) {
                Serial.println("Remote Command erfolgreich: " + target + " - " + action);
            } else {
                Serial.println("Remote Command fehlgeschlagen: " + String(httpCode));
            }
            
            http.end();
        }
    }
}

// Web Server Setup
void setupWebServer() {
    // Hauptseite
    server.on("/", []() {
        server.send(200, "text/html", htmlTemplate);
    });
    
    // API Endpoints
    server.on("/api/status", []() {
        server.send(200, "application/json", getStatusJson());
    });
    
    server.on("/api/localstatus", []() {
        DynamicJsonDocument doc(256);
        doc["serverName"] = serverName;
        doc["serverOnline"] = localServerStatus;
        doc["serverPower"] = localPowerStatus;
        doc["pingTime"] = localPingTime;
        
        String output;
        serializeJson(doc, output);
        server.send(200, "application/json", output);
    });
    
    server.on("/api/control", HTTP_POST, []() {
        if (server.hasArg("plain")) {
            DynamicJsonDocument doc(256);
            deserializeJson(doc, server.arg("plain"));
            
            String action = doc["action"].as<String>();
            handleRemoteCommand("local", action);
            
            server.send(200, "application/json", "{\"success\":true}");
        } else {
            server.send(400, "application/json", "{\"error\":\"No data\"}");
        }
    });
    
    server.on("/control", HTTP_POST, []() {
        if (server.hasArg("plain")) {
            DynamicJsonDocument doc(256);
            deserializeJson(doc, server.arg("plain"));
            
            String target = doc["target"].as<String>();
            String action = doc["action"].as<String>();
            
            handleRemoteCommand(target, action);
            
            server.send(200, "application/json", "{\"success\":true}");
        } else {
            server.send(400, "application/json", "{\"error\":\"No data\"}");
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
