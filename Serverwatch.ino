#include <WiFi.h>
#include <WebServer.h>
#include <WiFiClient.h>
#include <esp_task_wdt.h>
#include <esp_idf_version.h>

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
#define WEB_PASSWORD "serverwatch"        // Standard-Passwort: bitte in secrets.h ändern
#endif
const char* ssid = WIFI_SSID;
const char* password = WIFI_PASSWORD;
const char* webUser = WEB_USER;
const char* webPassword = WEB_PASSWORD;

// Server Name
const char* nodeName = "Heimserver";      // Name deines Servers

// Server IP-Adresse zum Prüfen
const char* serverIP = "192.168.178.1";   // Die Ip von dem Überwachenden Server
const int checkPort = 80;                 // Auf welchen Port soll geprüft werden (80=HTTP, 22=SSH, 445=SMB, etc.)

// GPIO Pins
const int POWER_CHECK_PIN = 4;  // Pin zum Prüfen der Spannung (Mainboard)
const int POWER_BUTTON_PIN = 3; // Pin zum Durchschalten (Startknopf)
const bool usePowerSense = true; // false, wenn POWER_CHECK_PIN nicht mit dem Mainboard verbunden ist
const bool reduceTxPower = true; // true senkt die WLAN-Sendeleistung auf 8,5 dBm (hilft z. B. beim ESP32-C3 Super Mini mit schwacher Antenne, Issue #23); false für volle Sendeleistung
const bool useLogin = false; // true, wenn der ESP außerhalb des privaten Netzes erreichbar ist (aktiviert Login per HTTP Basic Auth)
const char* localDomain = "fritz.box"; // zusätzliche lokale DNS-Domain des Routers, "" = keine

// Zeiteinstellungen
int onTime = 800; // Zeit wie lange der Ausgang bestromt werden soll in Millisekunden

const char* firmwareVersion = "1.0.13";

WebServer server(80);

// Status-Variablen für kontinuierliche Überwachung
bool lastServerStatus = false;
bool lastPowerStatus = false;
unsigned long lastCheck = 0;
const unsigned long checkInterval = 3000; // Alle 3 Sekunden prüfen
unsigned long lastWiFiCheck = 0;
const unsigned long wifiCheckInterval = 10000; // WiFi-Status alle 10 Sekunden prüfen
const int watchdogTimeoutS = 15;                  // Neustart, wenn loop() so lange (s) hängt
const unsigned long wifiRestartTimeout = 300000;  // Neustart, wenn WLAN so lange (ms) getrennt bleibt
const unsigned long wifiBootTimeout = 20000;      // So lange (ms) wartet setup() auf das WLAN
bool wifiLost = false;
String espHostname;
unsigned long wifiLostSince = 0;

// Diagnose (Issue #23): Zähler werden im WLAN-Event-Task geschrieben, daher volatile
volatile uint32_t wifiDisconnects = 0;
volatile uint8_t lastDisconnectReason = 0;
unsigned long maxLoopMs = 0;


bool checkServerReachable() {
  WiFiClient client;

  // Timeout explizit in ms: setTimeout() erwartet in arduino-esp32 2.x Sekunden (Issue #1)
  bool connected = client.connect(serverIP, checkPort, 1000);
  if (connected) {
    client.stop();
    return true;
  }
  return false;
}

bool checkServerStatus() {
  bool reachable = checkServerReachable();
  
  bool powerPresent = (digitalRead(POWER_CHECK_PIN) == HIGH);
  
  return reachable && powerPresent;
}

const char htmlPage[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="de">
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <title>Server Monitor - %NODE_NAME%</title>
    <link rel="icon" type="image/svg+xml" href="data:image/svg+xml,%3Csvg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 24 24' fill='none' stroke='%232563eb' stroke-width='2' stroke-linecap='round' stroke-linejoin='round'%3E%3Crect x='2' y='2' width='20' height='8' rx='2' ry='2'/%3E%3Crect x='2' y='14' width='20' height='8' rx='2' ry='2'/%3E%3Cline x1='6' y1='6' x2='6.01' y2='6'/%3E%3Cline x1='6' y1='18' x2='6.01' y2='18'/%3E%3C/svg%3E">
    <style>
        * {
            margin: 0;
            padding: 0;
            box-sizing: border-box;
        }
        body {
            background: #0a0a0a;
            color: #e0e0e0;
            font-family: -apple-system, BlinkMacSystemFont, 'Segoe UI', sans-serif;
            display: flex;
            justify-content: center;
            align-items: center;
            min-height: 100vh;
            padding: 20px;
        }
        .container {
            display: flex;
            gap: 20px;
            align-items: stretch;
            max-width: 700px;
            width: 100%;
        }
        .info-box {
            background: linear-gradient(145deg, #1a1a1a, #0f0f0f);
            border: 1px solid #2a2a2a;
            border-radius: 16px;
            padding: 30px;
            flex: 1;
            box-shadow: 0 8px 32px rgba(0,0,0,0.4);
        }
        .status-badge {
            display: inline-block;
            padding: 8px 16px;
            border-radius: 20px;
            font-size: 14px;
            font-weight: 600;
            margin-bottom: 20px;
        }
        .status-online {
            background: rgba(34, 197, 94, 0.2);
            color: #22c55e;
            border: 1px solid #22c55e;
        }
        .status-offline {
            background: rgba(239, 68, 68, 0.2);
            color: #ef4444;
            border: 1px solid #ef4444;
        }
        h1 {
            font-size: 24px;
            margin-bottom: 8px;
            color: #fff;
        }
        .info-line {
            padding: 12px 0;
            border-bottom: 1px solid #2a2a2a;
            display: flex;
            justify-content: space-between;
        }
        .info-line:last-child {
            border-bottom: none;
        }
        .label {
            color: #888;
            font-size: 14px;
        }
        .value {
            color: #e0e0e0;
            font-size: 14px;
            font-weight: 500;
        }
        .btn-container {
            display: flex;
            align-items: center;
        }
        .power-btn {
            background: linear-gradient(145deg, #2563eb, #1d4ed8);
            color: white;
            border: none;
            padding: 40px 30px;
            border-radius: 16px;
            font-size: 18px;
            font-weight: 600;
            cursor: pointer;
            transition: all 0.3s;
            box-shadow: 0 8px 32px rgba(37, 99, 235, 0.3);
            min-width: 150px;
        }
        .power-btn:hover {
            transform: translateY(-2px);
            box-shadow: 0 12px 40px rgba(37, 99, 235, 0.4);
        }
        .power-btn:active {
            transform: translateY(0);
        }
        .power-btn:disabled {
            background: #2a2a2a;
            cursor: not-allowed;
            box-shadow: none;
        }
        .esp-info {
            margin-top: 20px;
            padding-top: 20px;
            border-top: 1px solid #2a2a2a;
            font-size: 12px;
            color: #666;
            text-align: center;
        }
        @media (max-width: 600px) {
            .container {
                flex-direction: column;
            }
        }
    </style>
</head>
<body>
    <div class="container">
        <div class="info-box">
            <span id="statusBadge" class="status-badge">Prüfe...</span>
            <h1>Server Status</h1>
            <div class="info-line">
                <span class="label">Server Name</span>
                <span class="value">%NODE_NAME%</span>
            </div>
            <div class="info-line">
                <span class="label">Server IP</span>
                <span class="value">%SERVER_IP%:%SERVER_PORT%</span>
            </div>
            <div class="info-line">
                <span class="label">Erreichbarkeit</span>
                <span class="value" id="pingStatus">-</span>
            </div>
            <div class="info-line">
                <span class="label">Stromversorgung</span>
                <span class="value" id="powerStatus">-</span>
            </div>
            <div class="esp-info">
              ESP32-C3 • %ESP_IP%  <br><br>  Serverwatch by Gamerfreak_LP • ©2025 
            </div>
        </div>
        <div class="btn-container">
            <button class="power-btn" onclick="powerOn()">Server Starten</button>
        </div>
    </div>
    
    <script>
        function updateStatus() {
            fetch('/status')
                .then(r => r.json())
                .then(data => {
                    const badge = document.getElementById('statusBadge');
                    const pingStatus = document.getElementById('pingStatus');
                    const powerStatus = document.getElementById('powerStatus');
                    
                    if(data.online) {
                        badge.textContent = 'Online';
                        badge.className = 'status-badge status-online';
                    } else {
                        badge.textContent = 'Offline';
                        badge.className = 'status-badge status-offline';
                    }
                    
                    pingStatus.textContent = data.reachable ? 'Erreichbar' : 'Nicht erreichbar';
                    powerStatus.textContent = data.power === null ? 'Kein Sensor' : (data.power ? 'Vorhanden' : 'Nicht vorhanden');
                });
        }
        
        function powerOn() {
            const btn = event.target;
            btn.disabled = true;
            btn.textContent = 'Starte...';
            
            fetch('/poweron', {method: 'POST', headers: {'Content-Type': 'application/json'}, body: '{}'})
                .then(r => r.json())
                .then(data => {
                    btn.textContent = data.success ? 'Gestartet!' : (data.error || 'Fehler!');
                    setTimeout(() => {
                        btn.disabled = false;
                        btn.textContent = 'Server Starten';
                        updateStatus();
                    }, 2000);
                });
        }
        
        updateStatus();
        setInterval(updateStatus, 5000);
    </script>
</body>
</html>
)rawliteral";

// ================= Sicherheit (Issues #8, #9) =================

void sendError(int code, const char* message) {
  server.send(code, "application/json", String("{\"success\":false,\"error\":\"") + message + "\"}");
}

// Ist host (ggf. mit Port) eine Adresse dieses ESP? Schutz gegen DNS-Rebinding.
bool isOwnHost(String host) {
  host.toLowerCase();
  int colon = host.indexOf(':');
  if (colon >= 0) host = host.substring(0, colon);
  if (host.length() == 0) return false;
  if (host == WiFi.localIP().toString()) return true;
  String own = espHostname;
  own.toLowerCase();
  if (host == own || host == own + ".local") return true;
  String domain = localDomain;
  domain.toLowerCase();
  return domain.length() > 0 && host == own + "." + domain;
}

// Origin leer (kein Browser) oder eigene Adresse
bool isAllowedOrigin(const String& origin) {
  if (origin.length() == 0) return true;
  if (!origin.startsWith("http://")) return false;
  String host = origin.substring(7);
  if (host.indexOf('/') >= 0) return false;
  return isOwnHost(host);
}

// Bremse gegen Durchprobieren von Passwörtern: 1 s Sperre pro IP nach Fehlversuch
uint32_t failedLoginIp[8] = {};
unsigned long failedLoginAt[8] = {};
int failedLoginNext = 0;

// Host prüfen und Login verlangen (HTTP Basic Auth)
bool requireAuth() {
  if (!isOwnHost(server.hostHeader())) {
    sendError(403, "Unbekannter Host");
    return false;
  }
  if (!useLogin) return true;
  uint32_t ip = (uint32_t)server.client().remoteIP();
  for (int i = 0; i < 8; i++) {
    if (failedLoginIp[i] == ip && failedLoginAt[i] != 0 && millis() - failedLoginAt[i] < 1000) {
      sendError(429, "Zu viele Fehlversuche, bitte kurz warten");
      return false;
    }
  }
  if (server.authenticate(webUser, webPassword)) return true;
  if (server.hasHeader("Authorization")) {
    failedLoginIp[failedLoginNext] = ip;
    failedLoginAt[failedLoginNext] = millis() | 1;
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

// ================= Ende Sicherheit =================

void handleRoot() {
  if (!requireAuth()) return;
  String html = String(htmlPage);
  html.replace("%NODE_NAME%", nodeName);
  html.replace("%SERVER_IP%", serverIP);
  html.replace("%SERVER_PORT%", String(checkPort));
  html.replace("%ESP_IP%", WiFi.localIP().toString());
  server.send(200, "text/html", html);
}

void handleStatus() {
  if (!requireAuth()) return;
  // Nur die im loop() ermittelten Werte ausliefern, nicht pro Anfrage verbinden (Issue #1)
  bool reachable = lastServerStatus;
  bool powerOk = lastPowerStatus;
  bool online = reachable && powerOk;
  
  if (!usePowerSense) online = reachable;  // ohne Spannungsabgriff zählt allein die Erreichbarkeit (Issue #19)
  String json = "{\"online\":" + String(online ? "true" : "false") + 
                ",\"reachable\":" + String(reachable ? "true" : "false") + 
                ",\"power\":" + String(!usePowerSense ? "null" : powerOk ? "true" : "false") + "}";
  
  server.send(200, "application/json", json);
}

void handleDiag() {
  if (!requireAuth()) return;
  // Diagnose für Feldfehler wie "ESP nicht mehr erreichbar" (Issue #23)
  String json = "{\"uptime_ms\":" + String(millis()) +
                ",\"reset_reason\":" + String((int)esp_reset_reason()) +
                ",\"free_heap\":" + String(ESP.getFreeHeap()) +
                ",\"min_free_heap\":" + String(ESP.getMinFreeHeap()) +
                ",\"rssi\":" + String(WiFi.RSSI()) +
                ",\"wifi_disconnects\":" + String(wifiDisconnects) +
                ",\"last_disconnect_reason\":" + String(lastDisconnectReason) +
                ",\"max_loop_ms\":" + String(maxLoopMs) +
                ",\"version\":\"" + String(firmwareVersion) + "\"}";
  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "application/json", json);
}

void handlePowerOn() {
  if (!requireAuth() || !checkJsonWrite()) return;
  // Kein Power-Druck bei laufendem Server: das würde ihn herunterfahren (Issue #11)
  if (lastServerStatus || lastPowerStatus) {
    server.send(409, "application/json", "{\"success\":false,\"error\":\"Server läuft bereits\"}");
    return;
  }
  digitalWrite(POWER_BUTTON_PIN, HIGH);
  delay(onTime);
  digitalWrite(POWER_BUTTON_PIN, LOW);
  
  server.send(200, "application/json", "{\"success\":true}");
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

void setup() {
  Serial.begin(115200);
  
  pinMode(POWER_CHECK_PIN, INPUT_PULLDOWN);
  pinMode(POWER_BUTTON_PIN, OUTPUT);
  digitalWrite(POWER_BUTTON_PIN, LOW);
  
  delay(100);
  
  lastPowerStatus = usePowerSense && digitalRead(POWER_CHECK_PIN) == HIGH;
  lastServerStatus = false;
  
  Serial.println("\n--- ESP32 Server Monitor ---");
  Serial.print("POWER_CHECK_PIN (GPIO ");
  Serial.print(POWER_CHECK_PIN);
  Serial.print("): ");
  Serial.println(lastPowerStatus ? "HIGH (Spannung erkannt)" : "LOW (Keine Spannung)");
  
  Serial.print("Verbinde mit WiFi");
  espHostname = "ServerWatch-" + String(nodeName);
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
  Serial.print("Node Name: ");
  Serial.println(nodeName);
  
  server.on("/", handleRoot);
  server.on("/status", handleStatus);
  server.on("/api/diag", HTTP_GET, handleDiag);
  server.on("/poweron", HTTP_POST, handlePowerOn);  // nur POST: kein Auslösen per Link/<img> (Issue #9)
  
  const char* headerKeys[] = {"Content-Type", "Origin"};
  server.collectHeaders(headerKeys, 2);
  server.begin();
  if (useLogin && strcmp(webPassword, "serverwatch") == 0) Serial.println("WARNUNG: Standard-Passwort aktiv - WEB_PASSWORD in secrets.h setzen");
  Serial.println("Webserver gestartet!");

  // Watchdog: startet den ESP neu, wenn loop() hängt (Issue #4)
#if ESP_IDF_VERSION_MAJOR >= 5
  esp_task_wdt_config_t wdtConfig = { .timeout_ms = watchdogTimeoutS * 1000, .idle_core_mask = 0, .trigger_panic = true };
  esp_task_wdt_reconfigure(&wdtConfig);
#else
  esp_task_wdt_init(watchdogTimeoutS, true);
#endif
  enableLoopWDT();
}

void loop() {
  unsigned long loopStart = millis();  // Dauer dieses Durchlaufs für /api/diag (Issue #23)
  server.handleClient();
  
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
  
  if (currentMillis - lastCheck >= checkInterval) {
    lastCheck = currentMillis;
    
    bool currentPowerStatus = usePowerSense && digitalRead(POWER_CHECK_PIN) == HIGH;
    bool currentServerStatus = checkServerReachable();
    
    if (currentPowerStatus != lastPowerStatus) {
      Serial.print("Stromversorgung: ");
      Serial.println(currentPowerStatus ? "AN" : "AUS");
      lastPowerStatus = currentPowerStatus;
    }
    
    if (currentServerStatus != lastServerStatus) {
      Serial.print("Server erreichbar: ");
      Serial.println(currentServerStatus ? "JA" : "NEIN");
      lastServerStatus = currentServerStatus;
    }
  }
  
  unsigned long loopMs = millis() - loopStart;
  if (loopMs > maxLoopMs) maxLoopMs = loopMs;
}
