#include <WiFi.h>
#include <WebServer.h>
#include <WiFiClient.h>

// WiFi Zugangsdaten
const char* ssid = "DEIN_WLAN_NAME";      // Wlan SSID muss gesetzt werden
const char* password = "DEIN_WLAN_PASSWORT";      // Wlan Passwort muss gesetzt werden

// Server Name
const char* nodeName = "Heimserver";      // Name deines Servers

// Server IP-Adresse zum Prüfen
const char* serverIP = "192.168.178.1";   // Die Ip von dem Überwachenden Server
const int checkPort = 80;                 // Auf welchen Port soll geprüft werden (80=HTTP, 22=SSH, 445=SMB, etc.)

// GPIO Pins
const int POWER_CHECK_PIN = 4;  // Pin zum Prüfen der Spannung (Mainboard)
const int POWER_BUTTON_PIN = 3; // Pin zum Durchschalten (Startknopf)

// Zeiteinstellungen
int onTime = 800; // Zeit wie lange der Ausgang bestromt werden soll in Millisekunden

WebServer server(80);

// Status-Variablen für kontinuierliche Überwachung
bool lastServerStatus = false;
bool lastPowerStatus = false;
unsigned long lastCheck = 0;
const unsigned long checkInterval = 3000; // Alle 3 Sekunden prüfen

bool checkServerReachable() {
  WiFiClient client;
  client.setTimeout(1000);
  
  bool connected = client.connect(serverIP, checkPort);
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
                    powerStatus.textContent = data.power ? 'Vorhanden' : 'Nicht vorhanden';
                });
        }
        
        function powerOn() {
            const btn = event.target;
            btn.disabled = true;
            btn.textContent = 'Starte...';
            
            fetch('/poweron')
                .then(r => r.json())
                .then(data => {
                    btn.textContent = data.success ? 'Gestartet!' : 'Fehler!';
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

void handleRoot() {
  String html = String(htmlPage);
  html.replace("%NODE_NAME%", nodeName);
  html.replace("%SERVER_IP%", serverIP);
  html.replace("%SERVER_PORT%", String(checkPort));
  html.replace("%ESP_IP%", WiFi.localIP().toString());
  server.send(200, "text/html", html);
}

void handleStatus() {
  bool reachable = checkServerReachable();
  bool powerOk = digitalRead(POWER_CHECK_PIN) == HIGH;
  bool online = reachable && powerOk;
  
  String json = "{\"online\":" + String(online ? "true" : "false") + 
                ",\"reachable\":" + String(reachable ? "true" : "false") + 
                ",\"power\":" + String(powerOk ? "true" : "false") + "}";
  
  server.send(200, "application/json", json);
}

void handlePowerOn() {
  digitalWrite(POWER_BUTTON_PIN, HIGH);
  delay(onTime);
  digitalWrite(POWER_BUTTON_PIN, LOW);
  
  server.send(200, "application/json", "{\"success\":true}");
}

void setup() {
  Serial.begin(115200);
  
  pinMode(POWER_CHECK_PIN, INPUT_PULLDOWN);
  pinMode(POWER_BUTTON_PIN, OUTPUT);
  digitalWrite(POWER_BUTTON_PIN, LOW);
  
  delay(100);
  
  lastPowerStatus = (digitalRead(POWER_CHECK_PIN) == HIGH);
  lastServerStatus = false;
  
  Serial.println("\n--- ESP32 Server Monitor ---");
  Serial.print("POWER_CHECK_PIN (GPIO ");
  Serial.print(POWER_CHECK_PIN);
  Serial.print("): ");
  Serial.println(lastPowerStatus ? "HIGH (Spannung erkannt)" : "LOW (Keine Spannung)");
  
  Serial.print("Verbinde mit WiFi");
  WiFi.begin(ssid, password);
  
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  
  Serial.println("\nWiFi verbunden!");
  Serial.print("IP-Adresse: ");
  Serial.println(WiFi.localIP());
  Serial.print("Node Name: ");
  Serial.println(nodeName);
  
  server.on("/", handleRoot);
  server.on("/status", handleStatus);
  server.on("/poweron", handlePowerOn);
  
  server.begin();
  Serial.println("Webserver gestartet!");
}

void loop() {
  server.handleClient();
  
  unsigned long currentMillis = millis();
  if (currentMillis - lastCheck >= checkInterval) {
    lastCheck = currentMillis;
    
    bool currentPowerStatus = (digitalRead(POWER_CHECK_PIN) == HIGH);
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
}
