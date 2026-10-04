# Handoff-Notizen

Jede Übergabe zwischen Rechnern hängt hier einen neuen Abschnitt an (nicht
überschreiben). Neueste Einträge stehen unten.

## Übergabe Laptop → VibeCode-Server (2026-10-01)

**Auftrag von Alex (Originalwortlaut):** „arbeite nun mal alle Fehler ab. Lass dir dabei die Zeit, die du benötigst um die Arbeit perfekt zu machen. Falls du in das UsageLimit rein läufst, pausiere das ganze bis das Limit aufgehoben ist und führe dann selbstständig die Arbeit wieder fort“. Die Fehler sind die GitHub-Issues in CtrlCup/ServerWatch (`gh issue list`).

### Stand
- Erledigt und auf `main` gepusht (je ein Commit mit „Closes #…“, Version zuletzt **1.0.11**): #1–#21 außer #22. Commits: `git log --oneline cbd1d0b..main`.
- Suite: 127 Tests grün + Firmware-Build für den ESP32-C3. Auf dem Server laufen beide (venv unter `test/.venv`, siehe unten).

### Offen (in dieser Reihenfolge)
1. **#23 – Diagnose** (`gh issue view 23`): `WiFi.onEvent()` für STA_DISCONNECTED (Grund + `WiFi.disconnectReasonName()`), GOT_IP, LOST_IP zählen und seriell loggen. `GET /api/diag` in **beiden** Sketches, mit Login (`requireAuth()`), Felder: `uptime_ms`, `reset_reason`, `free_heap`, `min_free_heap`, `rssi`, `wifi_disconnects`, `last_disconnect_reason`, `max_loop_ms` (selbst gemessen), `version`. Keine Secrets/Token ausgeben. Option für reduzierte Sendeleistung, z. B. `const bool reduceTxPower = false;` → `WiFi.setTxPower(WIFI_POWER_8_5dBm)` nach `WiFi.begin()` (hilft oft beim ESP32-C3 Super Mini). Tests: in `test/tests/test_security.py` `/api/diag` wieder in `test_read_endpoints_require_authentication` und in `test_single_requires_authentication` aufnehmen (wurde bis #23 herausgenommen) und einen Test für die Felder in `test/tests/test_robustness.py` ergänzen (z. B. WLAN-Trennung mit Grund 15 simulieren → `wifi_disconnects >= 1`, `last_disconnect_reason == 15`). Die Simulation kann bereits `WiFi.onEvent`, `esp_reset_reason()`, `ESP.getFreeHeap()`, `WiFi.RSSI()` und `WiFi.setTxPower()`.
2. **#22 – Doku** (`gh issue view 22`): README überarbeiten. Neu hinzugekommen und unbedingt zu dokumentieren: `secrets.h` (Vorlage `secrets.example.h`, `WIFI_*`, `WEB_USER`/`WEB_PASSWORD`, `SWARM_KEY` ≥ 16 Zeichen, sonst Schwarm aus), Login/Standard-Passwort-Warnung, `usePowerSense`, neue Hostnamen `serverwatch-<name>-<mac6>`, Multi-API (`/api/status`, `/control`, `/api/wstoken`, `/api/diag`, Schwarm-Endpunkte `/api/nonce`, `/api/control`, `/api/localstatus` mit HMAC), WebSocket `ws://<esp>:81/?t=<token>`, `dashboard_html.h` wird mit `tools/embed_html.py` aus `serverwatch_Multi_interface.html` erzeugt. **Flash-Anleitung für PlatformIO:** Board `esp32-c3-devkitm-1`, `lib_deps` ArduinoJson ^6.21.3 + WebSockets ^2.4.1, `build_flags = -DARDUINO_USB_CDC_ON_BOOT=1`; in `src/` gehören der Sketch **plus** `dashboard_html.h` und `secrets.h`. Außerdem Pin-Hinweise (klassischer ESP32: GPIO 3 = RX, GPIO 5 Strapping) und die Test-Suite. `PROJECT_OVERVIEW.md` ebenfalls aktualisieren. Texte über den Agenten `writer` glätten lassen.
3. Danach: Prüfen, dass alle Issues geschlossen sind, und Alex fragen, ob ein **Minor-Bump** auf 1.1.0 gewünscht ist (viele neue Funktionen; laut Regeln nur nach Rückfrage). Bis dahin nur Patch-Bumps (`firmwareVersion` in **beiden** Sketches, je +0.0.1 pro Fix-Commit).

### Arbeitsweise (bisher bewährt)
- Pro Issue: Test zuerst (xfail-Marker entfernen mit `test/unmark_bug.py <key>`, Zuordnung in `test/tests/conftest.py` → `KNOWN_BUGS`, aktuell leer), dann Fix. `test/firmware/build.sh` und `test/run_tests.sh -q -m "not firmware"` müssen grün sein, dann Commit mit „Closes #N“.
- Nach globaler `CLAUDE.md`: nach jeder Änderung `scope-guard`, `regression-guard`, `reviewer`; vor jedem Push `release-guard`; bei Sicherheitsbezug zuerst `security`.
- **Branch:** Auf diesem vibe-Branch arbeiten und pushen (PR existiert). Nicht selbst nach `main` mergen und kein `vibe done`, den Merge entscheidet Alex.
- **Prototypen:** Typen (structs), die in Funktionssignaturen vorkommen, oben im Sketch definieren. Arduino-IDE und Simulation fügen Prototypen vor der ersten Funktion ein.
- **Dashboard:** Nur `serverwatch_Multi_interface.html` bearbeiten, dann `python3 tools/embed_html.py` ausführen. Ein Test prüft, dass der Header aktuell ist.
- **Simulation:** Fehlende Arduino-APIs in `test/sim/fakes/` ergänzen, dabei das Verhalten von arduino-esp32 2.0.17 möglichst genau nachbilden. Fremde Werte per `SWCFG_<name>` setzen (top-level `const char*`/`const bool`).
- **Gegenproben:** Bei Schutzmaßnahmen kurz zeigen, dass der Test ohne den Fix rot wird.

### Server-Umgebung (CT 170, Benutzer vibe)
- Python-Pakete liegen in `test/.venv` (pytest, requests, websockets, platformio). `test/run_tests.sh` nutzt die venv automatisch, `test/firmware/build.sh` nimmt `test/.venv/bin/pio`.
- `test/js/node_modules` (jsdom) ist installiert.
- Kein Chrome auf dem Server, Dashboard-Tests laufen über jsdom.
- Modell dieser Session: `claude-opus-5-5`, gesetzt in `.claude/settings.local.json` (nur lokal, per `.git/info/exclude` ausgeschlossen).
- Checkpoint für Usage-Limits: `~/.claude/resume/serverwatch.md` auf dem Server (Abschnitt 8 der globalen CLAUDE.md).

## Übergabe Laptop → VibeCode-Server: ServerTemp (2026-10-04)

**WICHTIG: Nicht selbstständig loslegen.** Alex wartet, bis sein wöchentliches Nutzungslimit zurückgesetzt ist. Diese Übergabe sichert nur den Stand. Arbeite erst weiter, wenn Alex ausdrücklich den Startauftrag gibt.

**Auftrag (Originalwortlaut, gekürzt):** „...einen Code, dass ich den auf einen ESP draufspielen kann und der dann eine Temperatur misst mit einem Temperatursensor und je nachdem wie die Temperatur ist, dann eine Kennlinie hat und dann einen Lüfter damit steuert. Und das soll es auch als Single und als Multi geben. Und im Multi soll der auch in dem Dashboard angezeigt werden von den anderen ESPs ... Drehzahl sehen ... an/aus ... Statusanzeige ... Drehzahl manuell hoch/runter, an/aus, Auto ... neuen Ordner ... ServerTemp ... README mit Flash-Erklärung ... automatisch im Serverwatch-Netzwerk erkennen ... ggf. Änderung bei Serverwatch.“

**Freigegebener Plan:** `.vibe/plan-servertemp.md` (inkl. Abschnitt „Sicherheitsvorgaben“, verbindlich). Entscheidungen von Alex stehen dort, zusätzlich: „Aus“ wird nie gespeichert (nach Neustart Auto).

### Stand
- [x] Commit 1 (Simulation: Unterordner-Sketches) und Commit 2 (Fakes DS18B20/LEDC/Tacho/Preferences): auf `main` (00813b7, 7a9266d).
- [~] Commit 3 ServerTemp Single, liegt auf diesem Branch als WIP (nicht auf main):
  - Firmware `ServerTemp/ServerTemp/ServerTemp.ino` (v1.0.0) mit `platformio.ini` und `secrets.example.h` ist fertig. Echter Build für den ESP32-C3: SUCCESS, Flash 62,6 %, RAM 12,8 %.
  - Tests `test/tests/test_servertemp.py`: 123/130 grün. Die 7 roten sind **Testfehler**, nicht Firmware-Fehler (vom Orchestrator geprüft):
    1. `test_device_name_xss_only_in_json_string` widerspricht `test_hostname_sanitized` (bereinigter Hostname enthält legitim „script“). Stattdessen prüfen: Hostname nur [a-z0-9-], deviceName kommt im JSON exakt als String zurück.
    2. `test_curve_points_below_on_between_above[fallend]` und `test_sensor_recovers_after_rescan`: Die Standardkurve ergibt bei 30 °C 37,5 % (Interpolation), nicht 30 %. Erwartung aus der Kurve berechnen.
    3. `test_overtemp_all_modes[auto|manual]` und `test_sensor_failure_failsafe_without_ramp`: Die Event-Basis wird genommen, während die Abwärtsrampe noch läuft. Erst auf einen stabilen Ausgang warten, dann prüfen, dass das erste neue ledc-Event 255 ist.
  - Flakes: `test_control_continues_without_wifi` (mit wait_for auf failsafe/pct warten), `test_out_of_range_is_invalid[126]` unter Last, `test_stall_after_5s_at_30_pct_or_more` (neu entdeckt, noch nicht analysiert).
  - Bereits behoben: Fake-Fehler `min`/`max` in `test/sim/fakes/Arduino.h` (dangling reference → Rückgabe per Wert). `simnet.expected_hostname(name, ip, prefix)`.
  - Noch zu tun in Commit 3: das Standard-Passwort in `ServerTemp.ino` (Zeilen ~30 und ~855) von „servertemp“ auf „serverwatch“ ändern, damit es in der ganzen Gerätefamilie einheitlich ist (gemeinsame secrets.h). Danach die Tests korrigieren (Rolle tester), dann scope-guard/regression-guard/reviewer, Commit „feat(servertemp): Einzel-Firmware“.
- [ ] Commit 4 Dashboard (Lüfterkarte, Kennlinien-Editor, Polling-Fallback, embed_html.py mit mehreren Zielen)
- [ ] Commit 5 ServerTemp Multi + test_code_sync.py
- [ ] Commit 6 ServerWatch 1.2.0 (Gerätetyp, Lüfterfelder, appendRemoteEntries) + test_mixed_swarm.py
- [ ] Commit 7 CI-Firmware-Build der ServerTemp-Sketches (`PLATFORMIO_WORKSPACE_DIR=test/.build/firmware/<name> pio run -d <sketchdir>`)
- [ ] Commit 8 Doku: `ServerTemp/README.md`, Verweise in README/PROJECT_OVERVIEW, KI-Brain (ESP32 TempManager ersetzt)

### Arbeitsweise
- Wie in der globalen CLAUDE.md: security → tester → developer → scope-guard/regression-guard/reviewer → release-guard vor jedem Push. Die Sicherheitsvorgaben aus dem Plan sind bereits erarbeitet.
- Fertige Commits nach den Guards auf **main** pushen (wie bisher). Den WIP-Branch danach aufräumen bzw. nach main mergen, sobald Commit 3 grün ist.
- Spezifikationsdetails für ServerTemp Single: siehe Docstrings und Asserts in `test/tests/test_servertemp.py` sowie den Plan. Klärungen des Orchestrators: „off“ sofort 0 (keine Rampe); Kurve 0 → Rampe auf 0, dann aus; temp null ohne gültigen Sensorwert; manPct-Standard 50; dc3-Kickstart 100 % für kickstartMs, dann Rampe.
- Checkpoint auf dem Server: `~/.claude/resume/serverwatch.md` (Abschnitt 8 der globalen CLAUDE.md).
- Testumgebung: `test/.venv` (python3-venv fehlt systemweit, venv per get-pip angelegt), `test/js/node_modules` (npm ci).
