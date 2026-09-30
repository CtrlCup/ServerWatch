# ServerWatch – Tests

Die Suite prüft beide Sketches, ohne dass echte ESP32 nötig sind:

| Teil | Was wird getestet | Wie |
|------|-------------------|-----|
| `tests/test_firmware.py` | Beide Sketches kompilieren für den ESP32-C3 | echte Toolchain via PlatformIO (`firmware/build.sh`) |
| `tests/test_swarm.py` | Multi-ESP-„Schwarm“: Erkennung per mDNS, Datenabgleich, Live-Updates, Fernsteuerung (Power/Reset/Shutdown) | Host-Simulation, mehrere Knoten |
| `tests/test_robustness.py` | Erreichbarkeit bei ausgeschaltetem Server, Blockaden im `loop()`, WLAN-Wiederverbindung, Boot ohne Router, Watchdog | Host-Simulation |
| `tests/test_security.py` | Authentifizierung, CSRF, Eingabeprüfung, WebSocket-Origin | Host-Simulation |
| `tests/test_single.py` | Einzel-Version `Serverwatch.ino` | Host-Simulation |
| `tests/test_dashboard.py` | Dashboard-JS: Rendering, WebSocket-Updates, XSS | Seite aus der Simulation, gerendert in jsdom |

## Ausführen

```bash
pip install -r test/requirements.txt      # pytest, requests, websockets
(cd test/js && npm ci)                    # jsdom für die Dashboard-Tests (optional, sonst skip)
test/run_tests.sh                         # alles (Firmware-Build nur, wenn `pio` installiert ist)
test/run_tests.sh -m "not firmware"       # ohne Firmware-Build
test/run_tests.sh -k swarm -v             # Auswahl
```

Voraussetzungen: Linux (Loopback-Adressen `127.0.10.x`), `g++` mit C++17, Python ≥ 3.9, optional Node.js ≥ 18 und PlatformIO.

## Bekannte Fehler (xfail)

Tests für Fehler, die als GitHub-Issue erfasst sind, tragen `@known_bug("<schlüssel>")`. Die Zuordnung Schlüssel → Issue-Nummer steht in `tests/conftest.py` (`KNOWN_BUGS`). Solche Tests sind `xfail(strict=True)`: Die Suite bleibt grün, solange der Fehler besteht. **Wird ein Fehler behoben, schlägt der zugehörige Test mit `XPASS` fehl.** Dann den Marker am Test und den Eintrag in `KNOWN_BUGS` entfernen.

Alle Tests ohne Rücksicht auf die Marker laufen lassen: `test/run_tests.sh --runxfail`.

## Wie die Simulation funktioniert

`sim/build.py` kompiliert den **unveränderten** Sketch-Code als Linux-Programm gegen nachgebaute Bibliotheken in `sim/fakes/` (`WiFi`, `WebServer`, `ESPmDNS`, `HTTPClient`, `WebSocketsServer`, FreeRTOS-Grundfunktionen, Task-Watchdog). Dabei wird nur mechanisch angepasst:

1. Top-level `const char* NAME = "…"` wird zu `sim_cfg("NAME", "…")`. So bekommt jeder Knoten per `SWCFG_NAME` eigene Werte, z. B. `SWCFG_serverName`.
2. Funktionsprototypen werden wie beim Arduino-Builder automatisch eingefügt.

Jeder simulierte ESP ist ein eigener Prozess mit eigener IP (`127.0.10.1`, `.2`, …). HTTP und WebSocket laufen über echte TCP-Sockets (Port = Originalport + `SW_PORT_OFFSET`). Das Verhalten folgt arduino-esp32 **2.0.17** (PlatformIO `framework-arduinoespressif32 3.20017`):

- `WebServer` ist synchron: ein Client pro `handleClient()`, blockierend.
- `WiFiClient::setTimeout()` nimmt **Sekunden**. Ein `connect()` zu einem ausgeschalteten Host blockiert, bis lwIP aufgibt (konservativ mit 45 s modelliert, `SIM_LWIP_SYN_GIVEUP_MS`).
- `MDNS.queryService()` blockiert 3 s (max. 20 Ergebnisse); Namenskonflikte werden wie bei ESP-IDF mit `-2` aufgelöst.
- `HTTPClient` hat 5 s Connect-Timeout, `setTimeout()` setzt den Lese-Timeout.
- WLAN-Modell mit Trennungsgründen, Auto-Reconnect-Regeln (`_isReconnectableReason`) und `first_connect`-Logik aus `WiFiGeneric.cpp`.
- Task-Watchdog (`esp_task_wdt_*`, `enableLoopWDT()`) und `ESP.restart()` starten den Prozess neu (Reset-Grund bleibt erhalten).

Zeit läuft beschleunigt (`SIM_TIME_SCALE`, Standard 5): `millis()`, `delay()` und alle modellierten Wartezeiten skalieren gemeinsam.

### Umgebung steuern (Zustandsverzeichnis `$SW_STATE`)

| Datei | Bedeutung |
|-------|-----------|
| `nodes/<ip>/ap` | `up` oder `down [trennungsgrund] [grund bei neuen versuchen]`, z. B. `down 200 201` |
| `nodes/<ip>/gpio_in_<pin>` | Eingangspegel, z. B. Pin 4 = Spannung vom Mainboard |
| `hosts/<ip>[_<port>]` | überwachter Server: `up`, `refused` (Host an, Port zu), `blackhole` (Host aus) |
| `nodes/<ip>/events.log` | GPIO-Schaltvorgänge, WLAN-Ereignisse, Neustarts |
| `nodes/<ip>/metrics` | u. a. `max_loop_ms` (längste `loop()`-Dauer) |
| `nodes/<ip>/serial.log` | serielle Ausgabe |

Weitere Schalter: `SIM_WIFI_CONNECT_MS`, `SIM_MDNS_QUERY_MS`, `SIM_HANG_AT_MS` (simulierter Hänger im ersten Boot).

### Grenzen

Die Simulation bildet Timing und API-Semantik nach, keine Funk-Physik, keinen Heap des ESP32 und keine Treiberfehler. Ob z. B. ein ESP32-C3-Super-Mini wegen seiner Antenne die Verbindung verliert, kann sie nicht zeigen. Erweitert ein Fix die Firmware um weitere Arduino-APIs, müssen die Fakes in `sim/fakes/` eventuell ergänzt werden. Die Fakes sind bewusst schlicht gehalten.
