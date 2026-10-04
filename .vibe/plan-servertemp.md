# Plan: ServerTemp – Lüftersteuerung in der ServerWatch-Familie

## Context
Alex möchte ein zweites Gerät für den Serverbereich: Ein ESP32-C3 Super Mini misst mit einem DS18B20 die Temperatur und regelt einen Lüfter nach einer Kennlinie. Wie bei ServerWatch soll es eine Single- und eine Multi-Variante geben. Die Multi-Variante tritt dem ServerWatch-Schwarm bei: ServerTemp- und ServerWatch-Geräte erkennen sich gegenseitig, zeigen sich gegenseitig im Dashboard an und lassen sich gegenseitig steuern. Lüftergeräte zeigen Drehzahl, Prozent, Temperatur und An/Aus-Status. Steuerbar sind Auto, Manuell (Drehzahl hoch/runter) und Aus. ServerTemp ersetzt das leere Repo `CtrlCup/ESP32-TempManager` (kein MQTT). Prioritäten: stabiles WLAN, immer erreichbar, schlanker Code, einfaches Flashen.

**Entscheidungen von Alex:**
- ServerTemp ersetzt TempManager, kein MQTT.
- Lüftertyp per Einstellung: `pwm4` (4-Pin-PWM mit Tacho) oder `dc3` (3-Pin über MOSFET).
- Je ein Sensor und ein Lüfter.
- Kennlinie im Dashboard änderbar, gespeichert im NVS.
- Sensorfehler im Modus „Aus“: der Lüfter bleibt aus, nur Warnung (in Auto und Manuell → 100 %).
- Pins: DS18B20 an GPIO 3, PWM an GPIO 5, Tacho an GPIO 6.
- Versionen: ServerWatch 1.2.0, ServerTemp 1.0.0.
- Bibliotheken: OneWire und DallasTemperature.

## Struktur
```
ServerTemp/
  README.md                 Verdrahtung (4-Pin / 3-Pin+MOSFET), Flashen (Arduino IDE + PlatformIO), Konfiguration, Schwarm, Failsafe
  ServerTemp/               Single (Ordner = Sketch-Name, direkt in der Arduino IDE zu öffnen)
    ServerTemp.ino, dashboard_html.h (erzeugt), secrets.example.h, platformio.ini (src_dir = .)
  ServerTemp_Multi/
    ServerTemp_Multi.ino, dashboard_html.h (erzeugt), secrets.example.h (mit SWARM_KEY), platformio.ini
```
- Die `platformio.ini` ist gepinnt auf `espressif32 @ 6.9.0`, also arduino-esp32 2.0.17.
  - Board `esp32-c3-devkitm-1`
  - `ARDUINO_USB_MODE=1`, `ARDUINO_USB_CDC_ON_BOOT=1`
  - `lib_deps`: OneWire, DallasTemperature, ArduinoJson 6; nur Multi zusätzlich WebSockets
- `.gitignore` bekommt `.pio/`. Die bestehende Regel `secrets.h` deckt auch die Unterordner ab.

## Wiederverwendung
- **Firmware:** `ServerTemp_Multi.ino` übernimmt die Schwarm-, Sicherheits- und WLAN-Funktionen aus `ServerWatch_Multi.ino` unverändert.
  - Der neue Test `test/tests/test_code_sync.py` vergleicht die Funktionstexte beider Sketches und schlägt bei Abweichungen fehl.
  - Betroffene Funktionen: sanitizeText, isValidNodeId, randomHex, isLowerHex, constantTimeEquals, hmacHex, isOwnHost, isAllowedOrigin, sendError, checkHost, loginThrottled, requireAuth, checkJsonWrite, issueNonce, consumeNonce, readJsonBody, sendActionResult, forwardRemoteAction, wsTokenFromUrl, webSocketEvent, sendStatusToClients, onWiFiEvent, startWiFi, setupWatchdog, scanForESPs, fetchRemoteStatus, appendRemoteEntries, struct RemoteESP.
  - So bleibt die getestete ServerWatch-Firmware fast unberührt, und Sicherheitsfixes können nicht in nur einer Kopie landen.
  - Keine Bibliothek und kein gemeinsamer Header (Flash-Einfachheit). Das ist eine spätere Option.
- **Dashboard:** eine Quelle `serverwatch_Multi_interface.html` für Server- und Lüfterkarten.
  - `tools/embed_html.py` schreibt den Header nach Root, `ServerTemp/ServerTemp/` und `ServerTemp/ServerTemp_Multi/`; `--check` prüft alle drei.
  - Neu: `createFanCard` (bei `type === "fan"`), Kennlinien-Editor nur auf der lokalen Lüfterkarte (Tabelle mit 2–6 Punkten und SVG-Vorschau), Polling-Fallback, wenn `/api/wstoken` mit 404 antwortet (Single).
  - `Serverwatch.ino` (Single, eigenes HTML) bleibt unverändert.

## Protokoll (abwärtskompatibel)
- **`/api/localstatus`:**
  - ServerTemp sendet `"type":"fan"`, `temp` (Zahl oder null), `sensorOk`, `rpm` (Zahl oder null), `pct`, `mode` (`auto`/`manual`/`off`), `fanOn`, `failsafe` (`""`, `"sensor"` oder `"overtemp"`), `stall`, `fanType`, `hasRpm`, dazu die gemeinsamen Felder.
  - Den Gerätenamen trägt `serverName`, damit ältere ServerWatch-Versionen wenigstens eine Karte zeigen.
  - ServerWatch sendet ab 1.2.0 `"type":"server"`. Fehlt `type`, gilt das Gerät als Server.
- **mDNS:** gleicher Dienst `_serverwatch._tcp` mit zusätzlichem TXT-Eintrag `type=fan`. Hostname `servertemp-<name>-<mac6>`; `makeHostname` bekommt den Präfix als Parameter.
- **Aktionen** im bestehenden signierten String:
  - `fan:auto`, `fan:manual`, `fan:off`, `fan:speed:N` mit N von 0 bis 100.
  - Strenge Prüfung, sonst 400.
  - Hoch/Runter sendet das Dashboard als `fan:speed:(pct±10)`, begrenzt auf 0–100.
  - ServerTemp lehnt Server-Aktionen mit 400 ab, ServerWatch lehnt `fan:*` über das bestehende `executeLocalAction` ab.
- **ServerWatch_Multi 1.2.0:**
  - `RemoteESP` bekommt `type` und die Lüfterfelder.
  - `fetchRemoteStatus` liest sie (Whitelist für type und mode, `sanitizeText`).
  - Der Remote-Teil von `getStatusJson` wird zu `appendRemoteEntries()` (wird synchronisiert).
  - Der lokale Eintrag und `localstatus` liefern `type:"server"`.
  - Lüftersteuerung läuft über das bestehende `/control` → `forwardRemoteAction`, dafür ist kein neuer Code nötig.

## Firmware ServerTemp
- **Konfiguration:**
  - `deviceName`, `fanType` (`"pwm4"`/`"dc3"`)
  - Pins `ONE_WIRE_PIN = 3`, `FAN_PWM_PIN = 5`, `FAN_TACH_PIN = 6`, `FAN_POWER_PIN = -1` (optional, MOSFET/Relais für echtes Aus bei pwm4)
  - `pwmInverted`
  - `emergencyTemp = 60` (5 K Hysterese), `fanMinPct = 20`, `kickstartMs = 1000`
  - aus ServerWatch: `useLogin = false`, `localDomain`, `reduceTxPower = true`, Watchdog, WLAN-Reconnect und Neustart, `/api/diag`
- **PWM:** LEDC mit 25 kHz und 8 Bit. Die Wrapper `fanPwmBegin`/`fanPwmWrite` kapseln die API-Unterschiede zwischen Core 2.x und 3.x per `ESP_ARDUINO_VERSION_MAJOR`.
  - Bei dc3 werden Werte unter `fanMinPct` zu 0; beim Anlauf gibt es einen Kickstart mit 100 % für 1 s.
- **Tacho:** FALLING-Interrupt (IRAM) mit Entprellung, Auswertung jede Sekunde, `rpm = pulses × 30`, geglättet.
  - Bei dc3 ist `rpm` nur bei 100 % gültig, sonst null.
  - `stall`: Lüfter steht 5 s lang bei mindestens 30 %, nur bei pwm4; nur Warnung.
- **DS18B20 nicht blockierend:** `setWaitForConversion(false)`, 11 Bit, alle 2 s, Auslesen nach 400 ms.
  - Ungültig sind −127, ein erster Wert von 85 und Werte außerhalb von −20…125 °C.
  - Nach 3 Fehlern gilt `sensorOk = false`; der Bus wird alle 30 s neu abgesucht.
- **Kennlinie:** 2–6 Punkte, linear interpoliert, außerhalb der Punkte konstant.
  - Glättung der Temperatur (EMA, α = 0,3) und eine Rampe am Ausgang (höchstens +10 %/s, −2 %/s).
  - Abschalten bei 0 %, wieder an erst bei t0 + 2 K.
  - Standard: (25,30), (35,45), (45,70), (55,100).
- **Failsafe** (Reihenfolge):
  1. Übertemperatur → 100 % in allen Modi, bis unter `emergencyTemp − 5`.
  2. Sensorfehler → 100 % in Auto und Manuell; in Aus bleibt der Lüfter aus, das Dashboard warnt.
  3. Boot → 100 % bis zum ersten gültigen Messwert.
  4. Ohne WLAN läuft die Regelung unverändert weiter.
- **NVS** (Preferences `servertemp`): `curve` (versionierter Blob), `mode`, `manPct`. Geschrieben wird nur bei Änderung und nach erfolgreicher Prüfung, Failsafe-Zustände nie; ungültige Inhalte ergeben den Standard.
- **Endpunkte:**
  - `/`, `/api/status` (lokaler Eintrag `type:"fan"`), `/api/wstoken` (nur Multi), `/api/diag`
  - `GET /api/curve`
  - `POST /api/curve` mit `requireAuth` und `checkJsonWrite`. Prüfung: 2–6 ganzzahlige Punkte, t von −10 bis 100 und streng steigend, p von 0 bis 100 und nicht fallend; sonst 400 mit Grund.
  - `POST /control`: lokal Lüfteraktionen, sonst Weiterleitung, also auch Server schalten aus dem ServerTemp-Dashboard.
  - Schwarm: `/api/localstatus`, `/api/nonce`, `/api/control`.
  - `monitorTask` macht nur den Scan.

## Sicherheitsvorgaben (Security-Review, verbindlich)
- **Notlauf:** Ab `emergencyTemp` sofort 100 %, ohne Rampe, unabhängig von Modus und Kennlinie. Ende erst unter `emergencyTemp − 5`. `emergencyTemp` ist nur im Code einstellbar, nicht über die API.
- **„Aus“ wird nicht gespeichert (Entscheidung von Alex):** Nach einem Neustart startet der Lüfter immer in Auto. Gespeichert werden Kennlinie, Modus auto/manual und der manuelle Wert.
- **NVS entprellen:** Geschrieben wird erst 30 s nach der letzten Änderung, höchstens einmal pro Minute und nur bei Änderung. Gilt für `manPct`, `mode` und `curve`. Test: 100× `fan:speed` in 10 s ergibt höchstens 2 Schreibvorgänge.
- **Strenger Parser:** `fan:speed:N` nimmt 1–3 Ziffern ohne Vorzeichen, ohne Leerzeichen und ohne führende Null (außer bei `0`), N von 0 bis 100. Die Aktion ist höchstens 16 Zeichen lang und groß-/kleinschreibungssensitiv, sonst 400. Gilt für `/control` und `/api/control`.
- **`fetchRemoteStatus`:**
  - Whitelist für `type` (`server`/`fan`, sonst `server`), `mode`, `failsafe` und `fanType`.
  - Zahlen mit Typ- und Bereichsprüfung: `temp` ist eine Zahl von −55 bis 125 oder null, `pct` 0–100, `rpm` 0–30000 oder null.
  - Ungültige Werte werden verworfen, nicht als 0 übernommen.
  - `doc.overflowed()` wird geprüft.
- **`GET /api/curve`:** steht hinter `requireAuth`, greift also nur bei `useLogin`. `POST /api/curve` lehnt einen Body über 1 KB ab (413/400).
- **README:**
  - Für Racks `useLogin = true` empfehlen, denn ohne Login kann jedes Gerät im LAN die Kühlung abschalten; der Notlauf schützt trotzdem.
  - Hardware-Hinweis: Ein Pull-Widerstand am PWM- bzw. MOSFET-Gate sorgt dafür, dass der Lüfter während Reset und Boot mit 100 % läuft.

## Simulation
- **`build.py`:** Sketches in Unterordnern; die Include-Reihenfolge wird pro Sketch gesetzt (`-I sketch_dir` statt globalem `-I ROOT`, Befund: sonst nähme ein Unterordner-Sketch den Root-Header).
- **Neue Fakes:**
  - `OneWire.h`
  - `DallasTemperature.h`: liest `nodes/<ip>/temp`, Werte z. B. `23.5`, `disconnected` oder `85`
  - LEDC (Event `ledc <pin> <duty>` und Datei `pwm_<pin>`)
  - `attachInterrupt` mit Puls-Thread nach `fan_rpm` bzw. einem Modell (Duty × 3000), `portMUX`/`IRAM_ATTR`
  - `Preferences.h` (Dateien pro Schlüssel, übersteht Neustart und kill/start, Zähler `nvs_writes`)
  - `esp_arduino_version.h`
- **`build.sh`:** baut die ServerTemp-Sketches mit der ausgelieferten `platformio.ini` (`PLATFORMIO_WORKSPACE_DIR` unter `test/.build`), die CI ebenso.
- **`simnet.py`:** `set_temp`, `set_fan_rpm`, `pwm`, `nvs`, `expected_hostname(prefix)`.

## Tests
- **`test_servertemp.py`** (beide Varianten):
  - Kennlinie: unterhalb, auf, zwischen und oberhalb der Punkte; Rampe; Abschalten und Hysterese; dc3 mit `fanMinPct` und Kickstart.
  - Modi auto, manual, off und speed; Aus mit bzw. ohne Power-Pin.
  - RPM und null bei dc3; `stall`.
  - Failsafe: Sensor getrennt; 85-Wert ignorieren; Übertemperatur auch in Aus und Manuell, Rückkehr unter 55 °C; Sensorfehler in Aus bleibt aus mit Warnung; Boot mit 100 %; Regelung ohne WLAN.
  - Persistenz über kill/start und ESP.restart; keine unnötigen NVS-Schreibvorgänge; kaputter NVS-Inhalt ergibt den Standard.
  - Validierung von `/api/curve` und den Aktionsstrings.
  - Sicherheit: optionaler Login, Host-Allowlist, CSRF, WS-Token, XSS über `deviceName`.
- **`test_mixed_swarm.py`** (2× ServerWatch_Multi, 1× ServerTemp_Multi):
  - Gegenseitige Erkennung und richtige `type`-Felder.
  - Lüfter von ServerWatch aus steuern, Server von ServerTemp aus schalten.
  - Gerät ohne `type` erscheint als Server.
  - Falsche Aktion je Typ ergibt 400; falscher Schlüssel oder Replay ergibt 403.
  - Statusantwort unter 700 Byte.
- **`test_code_sync.py`** und `embed_html.py --check` für alle drei Header.
- **`test_dashboard.py`:** Lüfterkarte (Temperatur, RPM bzw. „–“, %, Modus, Status An/Aus/Notlauf/Sensorfehler/Blockiert), Buttons senden die richtigen Aktionen inklusive Begrenzung, Editor nur lokal mit korrektem POST, Polling-Fallback; bestehende Server-Tests bleiben grün.
- **`test_firmware.py`:** alle vier Sketches kompilieren mit PlatformIO.

## Arbeitsweise und Commits
Vor dem Start legt `security` Risiken und Testvorgaben für die neuen Endpunkte und Aktionen fest. Pro Commit gilt: `tester` → `developer` → `scope-guard`, `regression-guard` und `reviewer` → `release-guard` → Push auf `main`. Checkpoint in `~/.claude/resume/serverwatch.md`.
1. Sim: Sketches in Unterordnern, Include-Reihenfolge (alle bestehenden Tests bleiben grün)
2. Sim: Fakes für Preferences, LEDC, Interrupt/Tacho, OneWire/Dallas
3. ServerTemp Single: Regelung, Failsafe, NVS, API, platformio.ini, Tests
4. Dashboard: Lüfterkarte, Editor, Polling-Fallback, `embed_html.py` mit mehreren Zielen, jsdom-Tests
5. ServerTemp Multi: Schwarm-Kopie, Aktionen, `test_code_sync.py`
6. ServerWatch 1.2.0: Gerätetyp, Lüfterfelder, `appendRemoteEntries`, `test_mixed_swarm.py`
7. CI: Firmware-Build der ServerTemp-Sketches
8. Doku: `ServerTemp/README.md`, Verweise in README und PROJECT_OVERVIEW, KI-Brain (TempManager ersetzt)

## Risiken
- **OneWire auf dem RISC-V-C3:** erst ab Version 2.3.8 stabil. Das Bit-Timing bildet die Simulation nicht ab → Test auf echter Hardware nötig.
- **4-Pin-Lüfter bei 0 %:** viele stoppen dann nicht. Abhilfe ist der optionale Power-Pin; `fanOn` wird zusätzlich über die RPM angezeigt.
- **Gemischte Versionen:** ServerWatch 1.1.0 zeigt Lüfter als Server-Karte an. Die README empfiehlt, alle Knoten auf 1.2.0 zu bringen.
- **Abdrift der kopierten Funktionen:** fängt der Sync-Test ab.
- **Core 3.x (LEDC-API):** per Guard abgedeckt, die CI testet aber nur 2.0.17.

## Bewusst nicht enthalten
- MQTT und Home Assistant
- mehrere Sensoren oder Lüfter
- Kennlinie aus der Ferne bearbeiten (über den Schwarm nur Modus und Drehzahl)
- PID-Regelung, Verlaufsgrafiken, OTA
- RPM bei 3-Pin-Lüftern
- Umbau von ServerWatch in eine Bibliothek
- Änderungen an `Serverwatch.ino`
- Archivieren des TempManager-Repos (macht Alex, nach außen wirksam)

## Verifikation
- `test/run_tests.sh` komplett grün, inklusive der neuen Dateien und des Firmware-Builds aller vier Sketches.
- CI auf GitHub grün.
- Manuell auf Hardware (Alex): Flashen nach `ServerTemp/README.md`, Temperatur und RPM im Dashboard, Kennlinie ändern und nach einem Neustart prüfen, dass sie gespeichert ist, gemischter Schwarm mit einem ServerWatch-ESP.
