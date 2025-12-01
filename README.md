<div align="center">

# 🖥️ ServerWatch

### ESP32-basierte Server-Überwachung mit Fernsteuerung

[![Arduino](https://img.shields.io/badge/Arduino-00979D?style=for-the-badge&logo=Arduino&logoColor=white)](https://www.arduino.cc/)
[![ESP32](https://img.shields.io/badge/ESP32-000000?style=for-the-badge&logo=espressif&logoColor=white)](https://www.espressif.com/)
[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg?style=for-the-badge)](https://opensource.org/licenses/MIT)

![GitHub Topics](https://img.shields.io/badge/Topics-esp32%20%7C%20iot%20%7C%20monitoring%20%7C%20arduino%20%7C%20home--automation-blue?style=for-the-badge)

Ein intelligentes Server-Monitoring-System basierend auf ESP32-C3, das die Verfügbarkeit deines Servers überwacht und eine Fernsteuerung des Power-Buttons ermöglicht.

[Features](#-features) •
[Screenshots](#-screenshots) •
[Hardware](#-hardware-anforderungen) •
[Installation](#-installation) •
[Konfiguration](#-konfiguration) •
[Verwendung](#-verwendung) •
[API](#-api-endpunkte)

</div>

---

## 📋 Übersicht

**ServerWatch** ist eine kostengünstige und elegante Lösung zur Überwachung und Fernsteuerung deines Heimservers oder PC. Der ESP32-C3 prüft kontinuierlich die Netzwerkerreichbarkeit und Stromversorgung und stellt diese Informationen über ein modernes, responsives Web-Interface zur Verfügung. Mit nur einem Klick kann der Server remote gestartet werden.

---

## 📸 Screenshots

<div align="center">

### Web-Interface (Desktop)
![Web Interface Desktop](docs/screenshots/interface-desktop.png)

### Mobile Ansicht
<img src="docs/screenshots/interface-mobile.png" width="300" alt="Mobile View">

### Status-Anzeige
![Status Display](docs/screenshots/status-display.png)

*Die Screenshots zeigen das moderne Dark-Theme Interface mit Echtzeit-Statusanzeige*

</div>

---

### ✨ Features

- 🌐 **Netzwerk-Monitoring** – Kontinuierliche Prüfung der Server-Erreichbarkeit über TCP
- ⚡ **Stromüberwachung** – Erkennung der Mainboard-Spannung über GPIO
- 🎛️ **Remote Power-On** – Ferngesteuertes Einschalten über GPIO-Simulation des Power-Buttons
- 📱 **Responsives Web-Interface** – Modernes Dark-Theme UI für Desktop & Mobile
- 🔄 **Auto-Refresh** – Status-Updates alle 5 Sekunden
- 📊 **Echtzeit-Status** – Live-Anzeige von Erreichbarkeit, Strom und Online-Status
- 🔌 **Minimale Hardware** – Nur ESP32-C3 + 2 GPIO-Pins erforderlich

---

## 🛠️ Hardware-Anforderungen

### Benötigte Komponenten

| Komponente | Beschreibung | Anzahl |
|------------|--------------|---------|
| **ESP32-C3** | Microcontroller mit WiFi | 1x |
| **Jumper Kabel** | Zum Verbinden mit Mainboard | 2-4x |
| **Optokoppler** (optional) | Zur galvanischen Trennung | 1x |
| **Widerstand** (optional) | Spannungsteiler falls >3.3V | 1-2x |

### GPIO-Belegung

```
GPIO 4  →  POWER_CHECK_PIN   (Eingang: Mainboard 3.3V/5V Standby)
GPIO 3  →  POWER_BUTTON_PIN  (Ausgang: Power-Button Pins am Mainboard)
```

### Verkabelung

```
┌─────────────┐                    ┌──────────────────┐
│   ESP32-C3  │                    │    Mainboard     │
│             │                    │                  │
│  GPIO 4 ◄───┼────────────────────┤ 3.3V/5V Standby  │
│             │                    │                  │
│  GPIO 3 ─────┼────────────────────┤ Power Button +   │
│             │                    │                  │
│  GND    ─────┼────────────────────┤ Power Button -   │
│             │                    │  (oder GND)      │
└─────────────┘                    └──────────────────┘
```

> ⚠️ **Wichtig:** Verwende Optokoppler für galvanische Trennung, um den ESP32 zu schützen!

---

## 🚀 Installation

### Voraussetzungen

- [Arduino IDE](https://www.arduino.cc/en/software) (Version 1.8.x oder 2.x)
- ESP32 Board-Support für Arduino IDE

### Arduino IDE einrichten

1. **ESP32 Boards hinzufügen:**
   - Öffne Arduino IDE → Einstellungen
   - Füge folgende URL bei "Zusätzliche Boardverwalter-URLs" hinzu:
     ```
     https://raw.githubusercontent.com/espressif/arduino-esp32/gh-pages/package_esp32_index.json
     ```

2. **ESP32 Board installieren:**
   - Werkzeuge → Board → Boardverwalter
   - Suche nach "ESP32" und installiere "esp32 by Espressif Systems"

3. **Board auswählen:**
   - Werkzeuge → Board → ESP32 Arduino → **ESP32C3 Dev Module**

### Software-Installation

1. **Repository klonen:**
   ```bash
   git clone https://github.com/CtrlCup/ServerWatch.git
   cd ServerWatch
   ```

   > 💡 **Tipp:** Wenn du das Repository auf GitHub forkst, vergiss nicht die Topics hinzuzufügen:
   > `esp32`, `iot`, `server-monitoring`, `arduino`, `home-automation`, `esp32-c3`, `remote-control`

2. **Projekt öffnen:**
   - Öffne `Serverwatch.ino` in der Arduino IDE

3. **Konfigurieren** (siehe [Konfiguration](#-konfiguration))

4. **Hochladen:**
   - Verbinde den ESP32 via USB
   - Wähle den richtigen Port: Werkzeuge → Port
   - Klicke auf "Hochladen" ➜

---

## ⚙️ Konfiguration

Bearbeite die folgenden Zeilen in `Serverwatch.ino`:

### 1️⃣ WiFi-Einstellungen

```cpp
const char* ssid = "DEIN_WLAN_NAME";          // ← Deine WLAN-SSID
const char* password = "DEIN_WLAN_PASSWORT";  // ← Dein WLAN-Passwort
```

### 2️⃣ Server-Konfiguration

```cpp
const char* nodeName = "Heimserver";          // ← Name deines Servers (erscheint im Web-Interface)
const char* serverIP = "192.168.178.1";       // ← IP-Adresse deines zu überwachenden Servers
const int checkPort = 80;                     // ← Port zum Prüfen (80=HTTP, 22=SSH, 445=SMB, etc.)
```

### 3️⃣ GPIO-Pins (falls abweichend)

```cpp
const int POWER_CHECK_PIN = 4;   // ← Pin zur Spannungsprüfung
const int POWER_BUTTON_PIN = 3;  // ← Pin zum Power-Button
```

### 4️⃣ Power-Button Timing

```cpp
int onTime = 800;  // ← Dauer des Button-Drucks in Millisekunden (Standard: 800ms)
```

---

## 💻 Verwendung

### Zugriff auf das Web-Interface

1. **ESP32 mit Strom versorgen**
2. **Warte auf WiFi-Verbindung** (blaue LED blinkt)
3. **IP-Adresse herausfinden:**
   - Über Arduino IDE Serial Monitor (115200 Baud)
   - Oder in deinem Router nach "ServerWatch-[NodeName]" suchen

4. **Browser öffnen und navigieren zu:**
   ```
   http://[ESP32-IP-ADRESSE]
   ```
   Beispiel: `http://192.168.178.50`

### Web-Interface Funktionen

<div align="center">

| Element | Funktion |
|---------|----------|
| **Status-Badge** | Zeigt Online/Offline Status |
| **Server Name** | Konfigurierter Name aus `nodeName` |
| **Server IP** | Überwachte Server-IP und Port |
| **Erreichbarkeit** | Netzwerk-Ping Status |
| **Stromversorgung** | GPIO-basierte Spannungsprüfung |
| **Server Starten Button** | Sendet Power-On Signal an Mainboard |

</div>

---

## 🔌 API-Endpunkte

Die folgenden REST-API Endpunkte sind verfügbar:

### `GET /`
Liefert das HTML Web-Interface

### `GET /status`
**Response (JSON):**
```json
{
  "online": true,
  "reachable": true,
  "power": true
}
```

| Feld | Typ | Beschreibung |
|------|-----|--------------|
| `online` | boolean | Server ist online UND mit Strom versorgt |
| `reachable` | boolean | Server ist über Netzwerk erreichbar |
| `power` | boolean | Mainboard-Spannung ist vorhanden |

### `GET /poweron`
Startet den Server durch Simulation eines Power-Button Drucks.

**Response (JSON):**
```json
{
  "success": true
}
```

---

## 🔧 Erweiterte Konfiguration

### Anpassung der Check-Intervalle

```cpp
const unsigned long checkInterval = 3000;  // Prüfung alle 3 Sekunden (in ms)
```

Im Web-Interface:
```javascript
setInterval(updateStatus, 5000);  // Auto-Refresh alle 5 Sekunden
```

### Verwendung verschiedener Ports

| Port | Service | Verwendungszweck |
|------|---------|------------------|
| 80 | HTTP | Standard Webserver |
| 22 | SSH | SSH-Verbindungen |
| 445 | SMB | Windows File Sharing |
| 3389 | RDP | Remote Desktop |
| 8080 | HTTP Alt | Alternative Webserver |

---

## 🐛 Troubleshooting

### ESP32 verbindet sich nicht mit WiFi
- ✅ Überprüfe SSID und Passwort in der Konfiguration
- ✅ Stelle sicher, dass der Router 2.4 GHz unterstützt (ESP32-C3 unterstützt kein 5 GHz)
- ✅ Prüfe die Serielle Ausgabe mit 115200 Baud

### Server-Status wird nicht korrekt angezeigt
- ✅ Überprüfe die `serverIP` und `checkPort` Einstellungen
- ✅ Stelle sicher, dass der Port auf dem Server offen ist
- ✅ Prüfe Firewall-Einstellungen

### Power-Button funktioniert nicht
- ✅ Prüfe die GPIO-Verkabelung zum Mainboard
- ✅ Erhöhe/verringere `onTime` (manche Mainboards benötigen länger)
- ✅ Teste mit einem Optokoppler für saubere Signalübertragung

### Web-Interface lädt nicht
- ✅ Überprüfe, ob der ESP32 mit dem WiFi verbunden ist
- ✅ Verwende die richtige IP-Adresse
- ✅ Lösche Browser-Cache

---

## 📊 Technische Details

### Spezifikationen

- **Microcontroller:** ESP32-C3 (RISC-V Single-Core @ 160MHz)
- **WiFi:** 802.11 b/g/n (2.4 GHz)
- **GPIO Spannung:** 3.3V (5V tolerant mit Schutz)
- **Webserver:** ESP32 WebServer Library
- **Sprache:** C++ (Arduino Framework)

### Abhängigkeiten

Die folgenden Bibliotheken werden verwendet (in ESP32 Arduino Core enthalten):
- `WiFi.h` – WiFi-Verbindung
- `WebServer.h` – HTTP-Server
- `WiFiClient.h` – TCP-Verbindungen

---

## 📝 Lizenz

Dieses Projekt ist unter der MIT-Lizenz lizenziert - siehe [LICENSE](LICENSE) Datei für Details.

---

## 👤 Autor

**CtrlCup (aka Gamerfreak_LP | ~Alex)**

- 🐙 **GitHub:** [@CtrlCup](https://github.com/CtrlCup)

---

## 🤝 Beitragen

Contributions, Issues und Feature Requests sind willkommen!

1. Fork das Projekt
2. Erstelle einen Feature Branch (`git checkout -b feature/AmazingFeature`)
3. Commit deine Änderungen (`git commit -m 'Add some AmazingFeature'`)
4. Push zum Branch (`git push origin feature/AmazingFeature`)
5. Öffne einen Pull Request

### 🏷️ GitHub Topics

Dieses Repository verwendet folgende Topics für bessere Auffindbarkeit:
- `esp32` - ESP32 Microcontroller
- `esp32-c3` - Spezifische ESP32-C3 Variante
- `iot` - Internet of Things
- `server-monitoring` - Server-Überwachung
- `arduino` - Arduino Framework
- `home-automation` - Heimautomatisierung
- `remote-control` - Fernsteuerung
- `webserver` - Eingebauter Webserver

---

## 🙏 Danksagungen

- ESP32 Community für die umfangreiche Dokumentation
- Arduino Team für das Framework
- Allen Contributors und Testern

---

<div align="center">

**Erstellt mit ❤️ für die Heimserver-Community**

⭐ **Wenn dir dieses Projekt gefällt, gib ihm einen Stern!** ⭐

</div>
