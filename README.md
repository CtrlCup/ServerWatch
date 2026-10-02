<div align="center">

# 🖥️ ServerWatch

### ESP32-basierte Server-Überwachung mit Fernsteuerung

[![Arduino](https://img.shields.io/badge/Arduino-00979D?style=for-the-badge&logo=Arduino&logoColor=white)](https://www.arduino.cc/)
[![ESP32](https://img.shields.io/badge/ESP32-000000?style=for-the-badge&logo=espressif&logoColor=white)](https://www.espressif.com/)
[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg?style=for-the-badge)](https://opensource.org/licenses/MIT)

![GitHub Topics](https://img.shields.io/badge/Topics-esp32%20%7C%20iot%20%7C%20monitoring%20%7C%20arduino%20%7C%20home--automation-blue?style=for-the-badge)

Ein Server-Monitoring-System auf Basis des ESP32-C3, das die Verfügbarkeit deines Servers überwacht und seinen Power-Button (bei der Multi-Version auch den Reset-Button) aus der Ferne bedient.

[Features](#-features) •
[Screenshots](#-screenshots) •
[Hardware](#-hardware-anforderungen) •
[Installation](#-installation) •
[Konfiguration](#-konfiguration) •
[Verwendung](#-verwendung) •
[API](#-api-endpunkte) •
[Sicherheit](#-sicherheit) •
[Troubleshooting](#-troubleshooting) •
[Tests](#-tests)

</div>

---

## 📋 Übersicht

**ServerWatch** ist eine günstige Lösung, um einen Heimserver oder PC zu überwachen und einzuschalten. Der ESP32-C3 prüft regelmäßig, ob der Server im Netzwerk antwortet und (optional) ob am Mainboard Spannung anliegt. Beides zeigt ein responsives Web-Interface an. Ein Klick genügt, um den Server zu starten.

Es gibt zwei Varianten, die du getrennt voneinander flashst:

| | Einzel-Version | Multi-Version |
|---|---|---|
| Sketch | `Serverwatch.ino` | `ServerWatch_Multi.ino` |
| Überwacht | ein Server | beliebig viele Server, je ein ESP pro Server |
| Bedienung | Power-Button | Power-Button, Shutdown, Reset-Button (optional) |
| Oberfläche | schlichte Statusseite | Kachel-Dashboard mit Dark/Light Mode und WebSocket |
| Zusätzliche Bibliotheken | keine | ArduinoJson, WebSockets |

### 🆕 Multi-ESP Version

Die erweiterte **ServerWatch_Multi.ino** bietet:
- 🔗 **Automatische ESP-Erkennung**: ESPs finden sich per mDNS (nur mit gesetztem `SWARM_KEY`, siehe [Konfiguration](#-konfiguration))
- 🌐 **Multi-Server Dashboard**: alle Server in einer Kachel-Ansicht
- 🔄 **Cross-ESP Control**: jeder Server lässt sich von jedem ESP aus steuern
- 🌓 **Dark/Light Mode**: Theme-Umschaltung im Dashboard
- ⚡ **WebSocket Updates**: Statusänderungen kommen ohne Neuladen an
- 📱 **Responsiv**: für Smartphone, Tablet und Desktop

---

## 📸 Screenshots

<div align="center">

### Web-Interface (Desktop)
![Web Interface Desktop](docs/screenshots/interface-desktop.png)

### Mobile Ansicht
<img src="docs/screenshots/interface-mobile.png" width="300" alt="Mobile View">

### Status-Anzeige
![Status Display](docs/screenshots/status-display.png)

*Die Screenshots zeigen das Dark-Theme mit Echtzeit-Statusanzeige*

</div>

---

## ✨ Features

- 🌐 **Netzwerk-Monitoring**: Prüfung der Server-Erreichbarkeit per TCP-Verbindung auf einen frei wählbaren Port
- ⚡ **Stromüberwachung**: Erkennung der Mainboard-Spannung über einen GPIO (abschaltbar mit `usePowerSense = false`)
- 🎛️ **Remote Power-On**: Der ESP simuliert einen Druck auf den Power-Button
- 🔐 **Login und Schutzmaßnahmen**: optionaler Login per HTTP Basic Auth (`useLogin`), CSRF-Schutz, DNS-Rebinding-Schutz (siehe [Sicherheit](#-sicherheit))
- 🛟 **Robust**: Watchdog, automatische WLAN-Wiederverbindung, Diagnose-Endpunkt `/api/diag`
- 📱 **Responsives Web-Interface**: Dark-Theme für Desktop und Mobil
- 🔄 **Auto-Refresh**: die Einzel-Version aktualisiert den Status alle 5 Sekunden, die Multi-Version bekommt ihn per WebSocket alle 3 Sekunden
- 🔌 **Wenig Hardware**: ein ESP32-C3 und zwei GPIO-Pins reichen

---

## 🛠️ Hardware-Anforderungen

### Benötigte Komponenten

| Komponente | Beschreibung | Anzahl |
|------------|--------------|---------|
| **ESP32-C3** | Microcontroller mit WiFi | 1x pro Server |
| **Optokoppler** (z. B. PC817) | Schaltet den Power-Button galvanisch getrennt | 1x (Multi mit Reset: 2x) |
| **Widerstand** ca. 330 Ω | Vorwiderstand für die Optokoppler-LED | 1x pro Optokoppler |
| **Spannungsteiler** (2 Widerstände) | Nur nötig, wenn die Spannungserkennung an 5 V Standby hängt | 2x |
| **Jumper Kabel** | Verbindung zum Mainboard | nach Bedarf |

### GPIO-Belegung

Die Standardwerte im Code sind für den ESP32-C3 gewählt:

```
GPIO 4  →  POWER_CHECK_PIN   (Eingang: Spannung vom Mainboard, über Spannungsteiler)
GPIO 3  →  POWER_BUTTON_PIN  (Ausgang: Power-Button, über Optokoppler)
GPIO 5  →  RESET_BUTTON_PIN  (Ausgang: Reset-Button, nur Multi, -1 = nicht benutzt)
```

Die Pins lassen sich oben im Sketch ändern. Dabei gilt:

- **ESP32-C3:** GPIO 2, 8 und 9 sind Strapping-Pins, beim Booten wird ihr Pegel ausgelesen. Verwende sie nicht als Ausgang zum Mainboard. GPIO 18 und 19 sind der USB-Anschluss.
- **Klassischer ESP32:** Die Standardpins passen hier nicht. GPIO 3 ist der UART0-RX (serielle Konsole), GPIO 5 ist ein Strapping-Pin. Als Ausgang bieten sich zum Beispiel GPIO 25, 26 und 27 an, als Eingang GPIO 34 oder 35. Diese beiden sind reine Eingänge und haben keinen internen Pulldown. Der Sketch ruft `pinMode(..., INPUT_PULLDOWN)` auf, das bleibt dort wirkungslos. Setze daher einen externen Pulldown (z. B. 10 kΩ nach GND) ein, sonst schwebt der Pin, wenn nichts angeschlossen ist. Das CI baut nur für den ESP32-C3, der klassische ESP32 ist nicht getestet.

### Verkabelung

Die Schaltungen zeigen gleich die empfohlene sichere Variante: einen Spannungsteiler für die Spannungserkennung und einen Optokoppler für den Power-Button.

**Spannungserkennung (GPIO 4)**

```
5V Standby (Mainboard) ──[ R1 10 kΩ ]──┬──[ R2 15 kΩ ]── GND (Mainboard und ESP)
                                       │
                                       └────────────── GPIO 4 (ESP32-C3)
```

Aus 5 V werden so etwa 3 V am Pin, das liegt unter den 3,3 V, die der ESP32 verträgt, und ist für ihn sicher als HIGH erkennbar. Bei 3,3 V Standby ist kein Teiler nötig.

**Power-Button (GPIO 3), Optokoppler PC817**

```
                    PC817
GPIO 3 ──[ 330 Ω ]──► 1 ┤├ 4 ───── Power-Button-Pin (+) am Mainboard
                      │    │
GND (ESP) ──────────── 2 ┤├ 3 ───── Power-Button-Pin (-) am Mainboard

(Pins 1/2 = LED-Seite, Pins 3/4 = Transistor-Seite; die Transistor-Seite liegt
 parallel zum Power-Taster des Gehäuses)
```

Pin 1 (Anode) bekommt das Signal über den Vorwiderstand, Pin 2 (Kathode) geht an GND des ESP. Pin 4 (Kollektor) gehört an den Anschluss, der im Ruhezustand die höhere Spannung führt, Pin 3 (Emitter) an den anderen. Für den Reset-Button (GPIO 5, nur Multi) baust du die gleiche Schaltung ein zweites Mal auf. Der Optokoppler schaltet wie ein Taster: Er verbindet die beiden Pins kurz miteinander, so wie es der Druck auf den Taster tut.

> ⚠️ **Wichtig:** Schalte GPIOs niemals direkt auf die Power-Button-Pins des Mainboards, sondern verwende einen Optokoppler (oder ein Relais). So bleibt die Taster-Strecke vom ESP getrennt. Die Spannungserkennung hat über den Teiler eine gemeinsame Masse mit dem ESP, sie ist also nicht galvanisch getrennt. Verbinde dafür GND von Mainboard und ESP. Ohne Spannungserkennung (`usePowerSense = false`) bleibt die Trennung vollständig.

> 💡 **Ohne Spannungsabgriff:** Ist `POWER_CHECK_PIN` nicht mit dem Mainboard verbunden, setze `usePowerSense = false`. Dann zählt allein die Erreichbarkeit im Netzwerk, und die Stromversorgung erscheint als „Kein Sensor“.

> 💡 **Stromversorgung des ESP:** Betreibe den ESP nicht am USB-Port des überwachten Servers. Der Port ist ausgeschaltet, wenn der Server aus ist, und kann bei Lastwechseln einbrechen. Ein eigenes Netzteil ist die sichere Wahl (siehe [Troubleshooting](#-troubleshooting)).

---

## 🚀 Installation

### Voraussetzungen

- [Arduino IDE](https://www.arduino.cc/en/software) (Version 1.8.x oder 2.x) oder [PlatformIO](https://platformio.org/)
- ESP32 Board-Support (`esp32 by Espressif Systems`). Entwickelt und getestet wurde mit arduino-esp32 2.0.17. Der Code enthält auch Anpassungen für Core 3.x.

### 📦 Bibliotheken

| Version | Bibliotheken |
|---------|--------------|
| Einzel (`Serverwatch.ino`) | nur der ESP32-Core (`WiFi`, `WebServer`, `WiFiClient`) |
| Multi (`ServerWatch_Multi.ino`) | zusätzlich **ArduinoJson** von Benoit Blanchon (6.x, getestet mit ^6.21.3) und **WebSockets** von Markus Sattler (getestet mit ^2.4.1) |

In der Arduino IDE findest du beide unter Werkzeuge → Bibliotheken verwalten. Installiere ArduinoJson in einer 6.x-Version, mit 7.x ist der Code nicht getestet.

### Arduino IDE einrichten

1. **ESP32 Boards hinzufügen:**
   - Arduino IDE → Einstellungen
   - Füge bei "Zusätzliche Boardverwalter-URLs" hinzu:
     ```
     https://raw.githubusercontent.com/espressif/arduino-esp32/gh-pages/package_esp32_index.json
     ```

2. **ESP32 Board installieren:**
   - Werkzeuge → Board → Boardverwalter
   - Suche nach "ESP32" und installiere "esp32 by Espressif Systems"

3. **Board auswählen:**
   - Werkzeuge → Board → ESP32 Arduino → **ESP32C3 Dev Module**
   - Werkzeuge → **USB CDC On Boot: Enabled**, sonst erscheint die serielle Ausgabe beim ESP32-C3 nicht im Serial Monitor

### Software-Installation

1. **Repository klonen:**
   ```bash
   git clone https://github.com/CtrlCup/ServerWatch.git
   cd ServerWatch
   ```

2. **Zugangsdaten anlegen:** Kopiere die Vorlage `secrets.example.h` nach `secrets.h` (beide liegen neben den `.ino`-Dateien) und trage deine Werte ein. `secrets.h` steht in der `.gitignore` und wird nie committet.
   ```bash
   cp secrets.example.h secrets.h
   ```

   | Eintrag | Bedeutung |
   |---------|-----------|
   | `WIFI_SSID`, `WIFI_PASSWORD` | dein WLAN (2,4 GHz) |
   | `WEB_USER`, `WEB_PASSWORD` | Login für das Web-Interface, nur mit `useLogin = true` (ohne `secrets.h` gilt `admin` / `serverwatch`, **bitte eigenes Passwort setzen**) |
   | `SWARM_KEY` | nur Multi: gemeinsamer Schlüssel aller ESPs, mindestens 16 Zeichen, auf allen ESPs identisch. Ohne gültigen Schlüssel bleibt der Schwarm (Erkennung und Fernsteuerung zwischen ESPs) ausgeschaltet. |

   Die WLAN-Daten stehen nicht mehr im Sketch. Fehlt `secrets.h`, kompiliert der Sketch mit Platzhaltern, der ESP verbindet sich dann aber nicht.

3. **Sketch-Ordner vorbereiten:** Die Arduino IDE erwartet, dass ein Sketch in einem gleichnamigen Ordner liegt. Weil im Repository beide `.ino`-Dateien nebeneinander liegen, würde die IDE sie zusammen kompilieren. Kopiere deshalb nur den gewünschten Sketch samt Headern in einen eigenen Ordner:

   ```
   Multi-Version:                          Einzel-Version:
   ServerWatch_Multi/                      Serverwatch/
   ├── ServerWatch_Multi.ino               ├── Serverwatch.ino
   ├── dashboard_html.h                    └── secrets.h
   └── secrets.h
   ```

   Die Multi-Version braucht `dashboard_html.h`, die Einzel-Version nicht.

4. **Hochladen:**
   - `.ino` im neuen Ordner in der Arduino IDE öffnen
   - ESP32 per USB verbinden und den Port wählen (Werkzeuge → Port)
   - Auf "Hochladen" klicken ➜

### 🧩 `dashboard_html.h`

Das Dashboard der Multi-Version liegt als `serverwatch_Multi_interface.html` im Repository. Daraus erzeugt `tools/embed_html.py` die Datei `dashboard_html.h`, die der Sketch einbindet:

```bash
python3 tools/embed_html.py          # Header neu erzeugen
python3 tools/embed_html.py --check  # nur prüfen, ob er aktuell ist
```

Wenn du am Dashboard etwas änderst, bearbeite nur die HTML-Datei und führe das Skript danach aus. Die Tests prüfen mit `--check`, dass der Header aktuell ist.

### ⚙️ Alternative: PlatformIO

Lege ein Projekt mit dieser Struktur an. Der Sketch bleibt eine `.ino`, `dashboard_html.h` und `secrets.h` gehören mit nach `src/`. Lege nur eine der beiden `.ino`-Dateien dort ab.

```
mein-projekt/
├── platformio.ini
└── src/
    ├── ServerWatch_Multi.ino
    ├── dashboard_html.h
    └── secrets.h
```

`platformio.ini` für die Multi-Version:

```ini
[env:esp32-c3]
platform = espressif32
board = esp32-c3-devkitm-1
framework = arduino
monitor_speed = 115200
build_flags =
    -DARDUINO_USB_MODE=1
    -DARDUINO_USB_CDC_ON_BOOT=1
lib_deps =
    bblanchon/ArduinoJson @ ^6.21.3
    links2004/WebSockets @ ^2.4.1
```

Für die Einzel-Version legst du `Serverwatch.ino` und `secrets.h` in `src/` und lässt `lib_deps` weg. Die beiden `build_flags` sorgen dafür, dass beim ESP32-C3 die serielle Ausgabe über USB kommt. `ARDUINO_USB_CDC_ON_BOOT=1` allein reicht nicht, ohne `ARDUINO_USB_MODE=1` bricht der Build mit „'Serial' was not declared“ ab.

```bash
pio run -t upload     # bauen und flashen
pio device monitor    # serielle Ausgabe (115200 Baud)
```

---

## ⚙️ Konfiguration

Neben den Zugangsdaten in `secrets.h` (siehe [Installation](#software-installation)) stellst du alles Weitere oben im jeweiligen Sketch ein.

### Standard Version (`Serverwatch.ino`)

```cpp
// Server
const char* nodeName = "Heimserver";      // Name deines Servers (Anzeige, Teil des Hostnamens)
const char* serverIP = "192.168.178.1";   // IP-Adresse des überwachten Servers
const int checkPort = 80;                 // Port für die Erreichbarkeitsprüfung (80=HTTP, 22=SSH, 445=SMB, ...)

// GPIO Pins
const int POWER_CHECK_PIN = 4;            // Eingang: Spannung vom Mainboard
const int POWER_BUTTON_PIN = 3;           // Ausgang: Power-Button
const bool usePowerSense = true;          // false, wenn POWER_CHECK_PIN nicht am Mainboard hängt
const bool reduceTxPower = true;          // true senkt die WLAN-Sendeleistung auf 8,5 dBm (Standard)
const bool useLogin = false;              // true: Login per HTTP Basic Auth, nötig wenn der ESP exponiert ist
const char* localDomain = "fritz.box";    // lokale DNS-Domain des Routers, "" = keine

// Zeit
int onTime = 800;                         // Dauer des Button-Drucks in ms
```

Der Hostname des ESP lautet `serverwatch-<name>-<mac6>` (aus `nodeName` gebildet: Kleinbuchstaben, Umlaute umgeschrieben, Leerzeichen und Sonderzeichen werden zu `-`, höchstens 20 Zeichen, dazu die letzten 6 Zeichen der MAC-Adresse). Bis Version 1.0.13 hieß er `ServerWatch-<nodeName>`. Alte Lesezeichen mit diesem Namen funktionieren nicht mehr, der ESP antwortet darauf mit 403.

### Multi-ESP Version (`ServerWatch_Multi.ino`)

```cpp
// Server
const char* serverName = "Heimserver";    // Name deines Servers (Anzeige, Teil des Hostnamens)
const char* serverIP = "192.168.178.1";   // IP-Adresse des überwachten Servers
const int serverCheckPort = 80;           // Port für die Erreichbarkeitsprüfung

// GPIO Pins
const int POWER_CHECK_PIN = 4;            // Eingang: Spannung vom Mainboard
const int POWER_BUTTON_PIN = 3;           // Ausgang: Power-Button
const int RESET_BUTTON_PIN = 5;           // Ausgang: Reset-Button, -1 wenn nicht benutzt
const bool usePowerSense = true;          // false, wenn POWER_CHECK_PIN nicht am Mainboard hängt
const bool reduceTxPower = true;          // true senkt die WLAN-Sendeleistung auf 8,5 dBm (Standard)
const bool useLogin = false;              // true: Login per HTTP Basic Auth, nötig wenn der ESP exponiert ist
const char* localDomain = "fritz.box";    // lokale DNS-Domain des Routers, "" = keine

// Timing
const int powerButtonTime = 800;          // Druckdauer Power-Button (ms)
const int resetButtonTime = 500;          // Druckdauer Reset-Button (ms)
const int scanInterval = 10000;           // Abstand der ESP-Suche (ms)
const int statusCheckInterval = 3000;     // Abstand der Status-Checks (ms)
const int pingTimeout = 1000;             // Timeout der Erreichbarkeitsprüfung (ms)
const int watchdogTimeoutS = 15;          // Watchdog-Zeit in Sekunden
const unsigned long wifiRestartTimeout = 300000;  // Neustart nach so vielen ms ohne WLAN

// mDNS
const char* mdnsServiceName = "serverwatch";      // Service-Name für die ESP-Erkennung
```

Der Hostname setzt sich zusammen als `serverwatch-<name>-<letzte 6 Stellen der MAC>`, zum Beispiel `serverwatch-heimserver-a1b2c3`. Der Name wird dabei in Kleinbuchstaben und gültige Hostname-Zeichen umgewandelt. Die MAC-Endung macht den Hostnamen eindeutig, auch wenn zwei ESPs denselben `serverName` tragen.

### Sendeleistung reduzieren

`reduceTxPower` ist standardmäßig an (`true`): Der ESP senkt die WLAN-Sendeleistung auf 8,5 dBm. Das kann bei Boards mit schwacher Antenne helfen, zum Beispiel beim ESP32-C3 Super Mini, wenn die Verbindung abreißt. Setze `false` für volle Sendeleistung, wenn dein Router weit entfernt ist. Bei Verbindungsabbrüchen siehe [Troubleshooting](#-troubleshooting).

---

## 💻 Verwendung

### Zugriff auf das Web-Interface

1. **ESP32 mit Strom versorgen**
2. **Auf die WLAN-Verbindung warten.** Der ESP versucht es beim Start bis zu 20 Sekunden, danach im Hintergrund weiter.
3. **IP-Adresse herausfinden:**
   - im Serial Monitor (115200 Baud)
   - oder in deinem Router nach dem Hostnamen suchen (`serverwatch-<name>-<mac6>`)

4. **Browser öffnen:**
   ```
   http://[ESP32-IP-ADRESSE]
   ```
   Beispiel: `http://192.168.178.50`

5. **Anmelden** mit `WEB_USER` und `WEB_PASSWORD` aus deiner `secrets.h`.

Rufst du die Seite über einen Namen auf, muss er zum ESP passen (Hostname oder IP). Fremde Hostnamen lehnt der ESP ab (siehe [Sicherheit](#-sicherheit)).

### Web-Interface der Einzel-Version

<div align="center">

| Element | Funktion |
|---------|----------|
| **Status-Badge** | Online oder Offline |
| **Server Name** | Konfigurierter Name aus `nodeName` |
| **Server IP** | Überwachte Server-IP und Port |
| **Erreichbarkeit** | Ergebnis der TCP-Prüfung |
| **Stromversorgung** | Spannung am Mainboard, bei `usePowerSense = false` „Kein Sensor“ |
| **Server Starten** | Drückt den Power-Button, solange der Server aus ist |

</div>

### Multi-Dashboard

Das Dashboard zeigt jeden Server als Kachel. Du kannst ihn einschalten, herunterfahren (kurzer Druck auf den Power-Button) und, wenn `RESET_BUTTON_PIN` gesetzt ist, zurücksetzen. Hinweise wie das aktive Standard-Passwort oder ein fehlender `SWARM_KEY` blendet das Dashboard selbst ein.

---

## 🔌 API-Endpunkte

Mit `useLogin = true` verlangen alle Dashboard-Endpunkte einen Login (HTTP Basic Auth mit `WEB_USER` und `WEB_PASSWORD`), standardmäßig (`useLogin = false`) ist kein Login nötig. Dazu gilt für beide Versionen:

- Schreibende Anfragen (POST) akzeptieren nur `Content-Type: application/json` (sonst 415) und nur den eigenen Origin oder gar keinen (sonst 403). Das schützt vor CSRF.
- Der `Host`-Header muss zum ESP gehören (IP, Hostname, `<hostname>.local` oder `<hostname>.<localDomain>`; `.local` löst nur die Multi-Version per mDNS auf), sonst antwortet der ESP mit 403. Das schützt vor DNS-Rebinding.
- Mit `useLogin = true` sperrt der ESP die absendende IP für 1 Sekunde (Antwort 429).

Beispiel mit `curl`:

```bash
curl -u admin:DEIN_PASSWORT http://192.168.178.50/status
curl -u admin:DEIN_PASSWORT -X POST -H "Content-Type: application/json" -d '{}' http://192.168.178.50/poweron
```

### Einzel-Version (`Serverwatch.ino`)

#### `GET /`
Liefert das HTML Web-Interface.

#### `GET /status`
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
| `online` | boolean | Server erreichbar **und** Spannung vorhanden. Bei `usePowerSense = false` zählt nur die Erreichbarkeit. |
| `reachable` | boolean | Server antwortet auf dem Prüfport |
| `power` | boolean oder `null` | Mainboard-Spannung vorhanden, `null` bei `usePowerSense = false` |

#### `POST /poweron`
Startet den Server durch einen Druck auf den Power-Button (`onTime` ms). Nur POST, mit `Content-Type: application/json` (und Login, wenn `useLogin = true`). Der Body wird nicht ausgewertet, `{}` genügt.

```json
{ "success": true }
```

Läuft der Server schon (erreichbar oder Spannung vorhanden), antwortet der ESP mit **409** und drückt nichts, denn ein Druck würde den Server herunterfahren:

```json
{ "success": false, "error": "Server läuft bereits" }
```

#### `GET /api/diag`
Diagnosedaten, siehe [Diagnose](#diagnose-apidiag).

### Multi-Version (`ServerWatch_Multi.ino`)

#### `GET /`
Liefert das Dashboard.

#### `GET /api/status`
Status aller bekannten Server:

```json
{
  "defaultCredentials": false,
  "swarmEnabled": true,
  "servers": {
    "local": {
      "isLocal": true,
      "id": "a1b2c3d4e5f6",
      "hostname": "serverwatch-heimserver-d4e5f6",
      "serverName": "Heimserver",
      "serverIP": "192.168.178.1",
      "serverPort": 80,
      "espIP": "192.168.178.50",
      "serverOnline": true,
      "serverPower": true,
      "espReachable": true,
      "pingTime": 4,
      "hasReset": true,
      "version": "1.1.0"
    },
    "112233445566": {
      "isLocal": false,
      "...": "wie oben, zusätzlich lastSeenAgo, uptime und rssi"
    }
  }
}
```

Der lokale Server steht unter dem Schlüssel `local`, andere ESPs unter ihrer ID (MAC ohne Doppelpunkte). `serverPower` ist `null`, wenn der betreffende ESP mit `usePowerSense = false` läuft. `lastSeenAgo` (ms seit der letzten Antwort), `uptime` (ms) und `rssi` kommen nur bei entfernten ESPs. Nicht erreichbare ESPs bleiben mit `espReachable: false` in der Liste und fallen nach 24 Stunden heraus.

#### `POST /control`
Löst eine Aktion aus. JSON und eigener Origin sind Pflicht, Login nur mit `useLogin = true`.

```json
{ "target": "local", "action": "power" }
```

| Feld | Werte |
|------|-------|
| `target` | `"local"` (dieser ESP) oder die ID eines anderen ESP aus `/api/status` |
| `action` | `"power"` (einschalten), `"shutdown"` (Power-Button drücken, um herunterzufahren), `"reset"` |

Antwort bei Erfolg: `{"success":true}`, sonst `{"success":false,"error":"..."}` mit diesen Codes:

| Code | Bedeutung |
|------|-----------|
| 400 | ungültiges JSON, kein Ziel oder unbekannte Aktion |
| 404 | unbekanntes Ziel |
| 409 | `power`, obwohl der Server läuft („Server läuft bereits“); `shutdown`, obwohl er aus ist („Server ist bereits aus“); `reset`, obwohl er aus ist („Server ist aus“); oder ein Taster wird gerade gedrückt |
| 501 | `reset`, aber `RESET_BUTTON_PIN` ist -1 |
| 504 | der Ziel-ESP antwortet nicht |

Als „läuft“ gilt, wenn Spannung anliegt oder der Server erreichbar ist. Fehler des Ziel-ESP werden bei Fernsteuerung durchgereicht.

#### `GET /api/wstoken`
Liefert den Token für den WebSocket, mit `Cache-Control: no-store`:

```json
{ "token": "..." }
```

Der Token ändert sich bei jedem Neustart des ESP.

#### `GET /api/diag`
Diagnosedaten, siehe [Diagnose](#diagnose-apidiag).

#### WebSocket `ws://<esp>:81/?t=<token>`
Der Port ist 81, der Token kommt aus `/api/wstoken`. Ohne gültigen Token trennt der ESP die Verbindung sofort. Der `Origin` des Handshakes muss leer oder die eigene Adresse sein. Direkt nach dem Verbinden und danach alle 3 Sekunden sendet der ESP:

```json
{ "type": "update", "defaultCredentials": false, "swarmEnabled": true, "servers": { "...": "wie bei /api/status" } }
```

Nachrichten vom Client wertet der ESP nicht aus.

#### Schwarm-Endpunkte (zwischen den ESPs)
Diese Endpunkte nutzen die ESPs untereinander. Sie brauchen keinen Login, sondern sind mit dem `SWARM_KEY` per HMAC-SHA256 gesichert. Ohne gültigen `SWARM_KEY` antworten sie mit 403 („Schwarm deaktiviert“).

| Endpunkt | Zweck |
|----------|-------|
| `GET /api/nonce` | Liefert eine Einmal-Nonce (`{"nonce":"..."}`, 32 Hex-Zeichen, 30 Sekunden gültig, nur einmal verwendbar) |
| `GET /api/localstatus?c=<challenge>` | Status dieses ESP als `{"payload":"<JSON-String>","sig":"<HMAC>"}`. `c` ist eine Zufallszahl des Abfragenden (32 Hex-Zeichen), die in die Signatur eingeht, damit alte Antworten nicht wiederverwendet werden können. |
| `POST /api/control` | Fernsteuerung durch einen anderen ESP: `{"action":"...","nonce":"...","sig":"..."}`. Die Signatur bindet die Ziel-ID, die Nonce und die Aktion. Bei falscher Signatur antwortet der ESP mit 403 („Ungültige Signatur“). |

Auch hier muss der `Host`-Header zum ESP passen.

### Diagnose (`/api/diag`)

Beide Versionen liefern unter `GET /api/diag` (Login nur mit `useLogin = true`, `Cache-Control: no-store`) Kennzahlen, mit denen sich Feldfehler eingrenzen lassen:

```json
{
  "uptime_ms": 86400000,
  "reset_reason": 1,
  "free_heap": 180000,
  "min_free_heap": 150000,
  "rssi": -61,
  "wifi_disconnects": 0,
  "last_disconnect_reason": 0,
  "max_loop_ms": 12,
  "version": "1.1.0"
}
```

| Feld | Bedeutung |
|------|-----------|
| `uptime_ms` | Laufzeit seit dem Start in Millisekunden |
| `reset_reason` | Grund des letzten Neustarts, Zahl von `esp_reset_reason()` (Tabelle unten) |
| `free_heap` | aktuell freier Heap in Bytes |
| `min_free_heap` | kleinster freier Heap seit dem Start |
| `rssi` | WLAN-Signalstärke in dBm |
| `wifi_disconnects` | Anzahl der WLAN-Trennungen seit dem Start, ohne die eigenen (Grund 8, vom Sketch beim Reconnect ausgelöst) |
| `last_disconnect_reason` | WLAN-Grundcode der letzten Trennung, 0 = keine. Den Klartext schreibt der ESP ins serielle Log. |
| `max_loop_ms` | längster `loop()`-Durchlauf seit dem Start in Millisekunden |
| `version` | Firmware-Version |

`reset_reason`, die wichtigsten Werte:

| Wert | Bedeutung |
|------|-----------|
| 1 | Einschalten (Power-on) |
| 3 | Software-Neustart (z. B. nach 5 Minuten ohne WLAN) |
| 4 | Panic (Absturz) |
| 5 | Interrupt-Watchdog |
| 6 | Task-Watchdog (`loop()` oder Monitor-Task hing, siehe [Robustheit](#-robustheit)) |
| 9 | Brownout (Spannungseinbruch) |

Ein Durchlauf von `loop()`, der dauerhaft hängt, taucht bei `max_loop_ms` nicht auf, weil er nie endet. Den fängt der Watchdog per Neustart ab. Spuren davon findest du dann im `reset_reason` (6).

---

## 🔒 Sicherheit

ServerWatch ist für ein privates Heimnetz gedacht. Der Login ist deshalb standardmäßig aus (`useLogin = false`). Ohne Login kann jedes Gerät im LAN den Server schalten, zum Beispiel per `curl`. Webseiten im Browser werden weiterhin durch die Host-, Origin- und JSON-Prüfung abgewehrt (CSRF- und Rebinding-Schutz, Issues #8 und #9). Setze `useLogin = true`, wenn der ESP außerhalb des privaten Netzes erreichbar ist. Konkret:

- **Login** (nur mit `useLogin = true`) per HTTP Basic Auth für alle Dashboard-Endpunkte, mit 1 Sekunde Sperre pro IP nach einem Fehlversuch
- **CSRF-Schutz:** schreibende Anfragen nur als JSON und nur vom eigenen Origin, `/poweron` nur per POST
- **DNS-Rebinding-Schutz:** Anfragen mit fremdem `Host`-Header lehnt der ESP mit 403 ab. Erlaubt sind IP, Hostname, `<hostname>.local` und `<hostname>.<localDomain>`
- **WebSocket** (Multi): Token pflicht, Origin wird geprüft
- **Schwarm** (Multi): HMAC-signierte Anfragen mit Einmal-Nonce, ohne `SWARM_KEY` abgeschaltet

Wichtig sind zwei Grenzen:

- ⚠️ **HTTP ist unverschlüsselt.** Benutzername und Passwort gehen bei Basic Auth (mit `useLogin = true`) im Klartext durchs LAN. Betreibe ServerWatch deshalb nur in einem vertrauenswürdigen Netz und gib den ESP **nicht** ins Internet frei. Brauchst du Zugriff von unterwegs, geht das über ein VPN (z. B. WireGuard).
- ⚠️ **Ändere mit `useLogin = true` das Standard-Passwort** `serverwatch`. Die Multi-Version warnt im Dashboard, solange es aktiv ist. Beide Versionen schreiben beim Start eine Warnung ins serielle Log.

---

## 🛟 Robustheit

- **Watchdog:** Hängt `loop()` (bei der Multi-Version auch die Monitor-Task) länger als 15 Sekunden (`watchdogTimeoutS`), startet der ESP neu.
- **WLAN-Wiederverbindung:** Alle 10 Sekunden prüft der ESP die Verbindung und baut sie bei Bedarf neu auf. Bleibt das WLAN 5 Minuten (`wifiRestartTimeout`) weg, startet der ESP neu.
- **Start ohne WLAN:** `setup()` wartet höchstens 20 Sekunden auf das WLAN, danach läuft der ESP weiter und verbindet sich im Hintergrund.
- **Status-Check:** In der Multi-Version laufen der Status-Check und die ESP-Suche in einer eigenen Task (`monitorTask`). Webserver und WebSocket bleiben dadurch ansprechbar, auch wenn der überwachte Server nicht antwortet. Die Einzel-Version prüft im `loop()` mit 1 Sekunde Timeout, die Oberfläche kann dabei kurz verzögert reagieren.
- **Diagnose:** `/api/diag` liefert Reset-Grund, WLAN-Trennungen und Heap-Werte.

---

## 🔧 Erweiterte Konfiguration

### Anpassung der Check-Intervalle

Einzel-Version:

```cpp
const unsigned long checkInterval = 3000;  // Prüfung alle 3 Sekunden (in ms)
```

Multi-Version: `statusCheckInterval` (siehe oben).

Im Web-Interface der Einzel-Version:
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
- ✅ Überprüfe SSID und Passwort in der `secrets.h`
- ✅ Stelle sicher, dass der Router 2,4 GHz anbietet (der ESP32-C3 unterstützt kein 5 GHz)
- ✅ Prüfe die serielle Ausgabe mit 115200 Baud (beim ESP32-C3 muss „USB CDC On Boot“ aktiv sein)

### Server-Status wird nicht korrekt angezeigt
- ✅ Überprüfe `serverIP` und den Prüfport (`checkPort` bzw. `serverCheckPort`)
- ✅ Stelle sicher, dass der Port auf dem Server offen ist
- ✅ Prüfe Firewall-Einstellungen
- ✅ Ohne Spannungsabgriff `usePowerSense = false` setzen

### Power-Button funktioniert nicht
- ✅ Prüfe die Verkabelung des Optokopplers, besonders die Polung der Transistor-Seite
- ✅ Erhöhe oder verringere `onTime` bzw. `powerButtonTime` (manche Mainboards brauchen länger)
- ✅ Beim klassischen ESP32 darf der Ausgang kein Pin mit Sonderfunktion sein (siehe [GPIO-Belegung](#gpio-belegung))
- ✅ Der ESP drückt nicht, wenn der Server als „läuft bereits“ gilt (Antwort 409)

### Web-Interface lädt nicht oder fragt immer wieder nach dem Passwort
- ✅ Überprüfe, ob der ESP32 mit dem WiFi verbunden ist
- ✅ Verwende die IP-Adresse oder den Hostnamen des ESP, andere Namen lehnt er ab (403)
- ✅ Nur mit `useLogin = true`: Prüfe `WEB_USER` und `WEB_PASSWORD` in der `secrets.h`
- ✅ Lösche den Browser-Cache

### ESP nach einiger Zeit nicht erreichbar

Wenn der ESP nach Stunden oder Tagen nicht mehr antwortet, hilft diese Reihenfolge:

1. **Frag `/api/diag` ab**, solange der ESP noch antwortet, oder sobald er wieder erreichbar ist. Auffällig sind viele `wifi_disconnects`, ein schlechter `rssi`, ein kleiner `min_free_heap` oder ein hoher `max_loop_ms`.
2. **Reagiert er nicht mehr:** Prüfe, ob der ESP auf einen ICMP-Ping antwortet (`ping <ESP-IP>`) und ob ihn deine Fritzbox (oder dein Router) noch als verbunden anzeigt. Antwortet er auf Ping, aber nicht per HTTP, liegt das Problem im Sketch. Ist er auch im Router weg, hat er das WLAN verloren oder hat keinen Strom.
3. **Stecke ihn aus und wieder ein** und lies danach `reset_reason` aus `/api/diag`. Ein Wert von 9 (Brownout) deutet auf die Stromversorgung hin, 6 oder 5 auf einen Hänger (Watchdog), 1 auf einen normalen Start nach Stromverlust.
4. **Prüfe, ob der überwachte Server aus war.** Hängt der ESP am USB des Servers, verliert er mit ihm den Strom.
5. **Stromversorgung:** Betreibe den ESP nicht am USB-Port des überwachten Servers, sondern an einem eigenen, stabilen Netzteil. Spannungseinbrüche führen zu Brownouts.
6. **Sendeleistung:** Bei Boards mit schwacher Antenne (z. B. ESP32-C3 Super Mini) hilft die reduzierte Sendeleistung (`reduceTxPower = true`, Standard). Bei Abbrüchen nicht auf `false` stellen. Der Grund einer Trennung steht im seriellen Log und als Code in `last_disconnect_reason`.

---

## 🧪 Tests

Das Repository enthält eine Test-Suite, die ohne echte ESP32 auskommt. Sie simuliert beide Sketches auf dem Host (Authentifizierung, CSRF, Schwarm, Robustheit, Dashboard) und baut beide Sketches mit der echten Toolchain für den ESP32-C3.

```bash
pip install -r test/requirements.txt
(cd test/js && npm ci)                    # für die Dashboard-Tests
test/run_tests.sh                         # alles (Firmware-Build nur mit installiertem PlatformIO)
test/run_tests.sh -m "not firmware"       # ohne Firmware-Build
```

Details zu Aufbau, Simulation und bekannten Grenzen stehen in [test/README.md](test/README.md). Die GitHub-Action [.github/workflows/tests.yml](.github/workflows/tests.yml) führt bei jedem Push und Pull Request die Simulation und den Firmware-Build aus.

---

## 📊 Technische Details

### Spezifikationen

- **Microcontroller:** ESP32-C3 (RISC-V Single-Core @ 160 MHz)
- **WiFi:** 802.11 b/g/n (2,4 GHz)
- **GPIO Spannung:** 3,3 V, **nicht** 5 V tolerant. Signale mit mehr als 3,3 V brauchen einen Spannungsteiler.
- **Webserver:** Arduino `WebServer` (HTTP, Port 80), in der Multi-Version zusätzlich ein WebSocket-Server auf Port 81
- **Sprache:** C++ (Arduino Framework)

### Abhängigkeiten

**Einzel-Version:** nur der ESP32-Core.
- `WiFi.h`: WiFi-Verbindung
- `WebServer.h`: HTTP-Server
- `WiFiClient.h`: TCP-Verbindungen

**Multi-Version:** zusätzlich zum ESP32-Core (`ESPmDNS.h`, `HTTPClient.h`)
- **ArduinoJson** 6.x (^6.21.3)
- **WebSockets** von Markus Sattler (^2.4.1)

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

Lass vor dem Pull Request die [Tests](#-tests) laufen.

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
