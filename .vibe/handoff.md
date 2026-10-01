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
