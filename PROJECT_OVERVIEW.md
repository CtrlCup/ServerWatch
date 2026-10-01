# Project Overview & Purpose

**ServerWatch** is an ESP32-C3 based server monitoring and remote control system. It continuously checks a local server's network availability (TCP connect to a configurable port) and, optionally, its power state (mainboard standby voltage on a GPIO pin). It can also press the server's power button (and, in the multi-node variant, the reset button) through GPIO pins, ideally via optocouplers.

The repository provides two variants, flashed separately:
1. **Single-Node (`Serverwatch.ino`)**: A standalone HTTP web server that monitors one server and serves a simple web UI. It only needs the ESP32 core.
2. **Multi-Node (`ServerWatch_Multi.ino`)**: A dashboard for several servers (one ESP per server). It adds WebSocket updates, mDNS discovery of other ESPs ("swarm"), JSON communication and signed ESP-to-ESP requests. It needs ArduinoJson 6.x and the WebSockets library.

The user-facing documentation (German) is in [README.md](README.md).

---

## Tech Stack

- **Microcontroller**: ESP32-C3 (default pin assignment). Other ESP32 variants need different pins, see the README.
- **Languages**: C++ (Arduino framework), HTML/CSS/JavaScript (dashboard), Python and Node.js (tooling and tests)
- **Core libraries** (ESP32 Arduino core): `WiFi.h`, `WebServer.h`, `WiFiClient.h`, `esp_task_wdt.h`
  - Multi-Node only: `ESPmDNS.h`, `HTTPClient.h`, `mbedtls/md.h` (HMAC-SHA256)
- **External libraries (Multi-Node only)**:
  - `ArduinoJson` 6.x (`bblanchon/ArduinoJson @ ^6.21.3`)
  - `WebSockets` by Markus Sattler (`links2004/WebSockets @ ^2.4.1`)
- **Tested against**: arduino-esp32 2.0.17 (the code also handles ESP-IDF 5 / core 3.x via `ESP_IDF_VERSION_MAJOR`)

---

## Folder Structure & Key Files

```
ServerWatch/
├── Serverwatch.ino                   # Single-node sketch (HTTP API + embedded UI)
├── ServerWatch_Multi.ino             # Multi-node sketch (WebSocket + mDNS + swarm)
├── serverwatch_Multi_interface.html  # Dashboard source (the only place to edit the multi-node UI)
├── dashboard_html.h                  # Generated from the HTML file, included by ServerWatch_Multi.ino
├── secrets.example.h                 # Template for secrets.h (WiFi, login, swarm key)
├── secrets.h                         # Local credentials, in .gitignore (not in the repo)
├── tools/
│   └── embed_html.py                 # Generates dashboard_html.h (--check verifies it is current)
├── test/                             # Test suite (see test/README.md)
│   ├── run_tests.sh                  # Entry point
│   ├── tests/                        # pytest: single, swarm, robustness, security, dashboard, firmware
│   ├── sim/                          # Host simulation: sketches compiled on Linux against fake Arduino libraries
│   ├── firmware/build.sh             # Real firmware build for the ESP32-C3 via PlatformIO
│   └── js/                           # jsdom based dashboard rendering
├── .github/workflows/tests.yml       # CI: simulation tests and firmware build
├── docs/screenshots/                 # Screenshots used in the README
├── README.md                         # User guide (German)
├── PROJECT_OVERVIEW.md               # This file
├── AGENTS.md, CLAUDE.md              # Multi-machine agent workflow
└── LICENSE                           # MIT License
```

- **[Serverwatch.ino](Serverwatch.ino)**: Single-node firmware.
- **[ServerWatch_Multi.ino](ServerWatch_Multi.ino)**: Multi-node firmware.
- **[serverwatch_Multi_interface.html](serverwatch_Multi_interface.html)**: Dashboard source. Run `python3 tools/embed_html.py` after every change. The tests call it with `--check` and fail if [dashboard_html.h](dashboard_html.h) is stale.
- **[secrets.example.h](secrets.example.h)**: Copy to `secrets.h` next to the sketches. Defines `WIFI_SSID`, `WIFI_PASSWORD`, `WEB_USER`, `WEB_PASSWORD` and (multi-node only) `SWARM_KEY`. Without a swarm key of at least 16 characters the swarm stays disabled.
- **[tools/embed_html.py](tools/embed_html.py)**: Minifies the dashboard HTML and writes it into a raw string literal (`htmlTemplate`).
- **[test/README.md](test/README.md)**: How the test suite and the simulation work.

Arduino IDE note: a sketch must live in a folder of the same name, and two `.ino` files in one folder are compiled together. To build one variant, copy its `.ino` plus `secrets.h` (and `dashboard_html.h` for multi-node) into its own folder. With PlatformIO the same files go into `src/`.

---

## Main Entry Points & Core Logic

### 1. `Serverwatch.ino`
- **`setup()`**: Initializes the GPIOs (`POWER_CHECK_PIN` as `INPUT_PULLDOWN`, `POWER_BUTTON_PIN` as output), sets the hostname `ServerWatch-<nodeName>`, registers the WiFi event handler, connects to WiFi (waits at most 20 s), registers the routes and starts the web server and the task watchdog.
- **`loop()`**: Serves HTTP clients, checks the WiFi connection every 10 s (reconnect, restart after 5 minutes without WiFi) and every 3 s refreshes the stored reachability and power values with a TCP connect (1 s timeout). It also tracks the longest loop run for `/api/diag`.
- **Routes**: `GET /`, `GET /status`, `POST /poweron`, `GET /api/diag`.
- **`handlePowerOn()`**: Refuses with 409 if the server is already running, otherwise drives the pin HIGH for `onTime` ms (blocking).

### 2. `ServerWatch_Multi.ino`
- **`setup()`**: GPIOs (power, optional reset), WebSocket token, swarm switch (`SWARM_KEY` of at least 16 characters), WiFi (hostname `serverwatch-<name>-<last 6 MAC digits>` from `makeHostname()`), mDNS, web server, WebSocket server, watchdog and the `monitorTask`.
- **`monitorTask()`**: Own FreeRTOS task. Every `statusCheckInterval` (3 s) it runs `updateLocalStatus()` (TCP connect, power pin) and, if the swarm is enabled, `scanForESPs()` every `scanInterval` (10 s plus up to 3 s jitter). All blocking work (TCP connect, mDNS query, HTTP requests to other ESPs) happens here, so `loop()` never blocks. Shared state (`remoteESPs`) is protected by `remoteMutex`.
- **`loop()`**: Serves HTTP and WebSocket, releases the pressed button when its time is up (`releaseButtonIfDue()`, buttons are pressed non-blocking), checks WiFi, broadcasts the status to authenticated WebSocket clients every `statusCheckInterval` and tracks the longest loop run.
- **`scanForESPs()`**: Finds other ESPs via `MDNS.queryService`, plus all known ones, fetches `/api/localstatus` with a random challenge and accepts only answers with a valid HMAC. Unreachable ESPs are kept as "not reachable" and forgotten after 24 h.
- **`executeLocalAction()` / `forwardRemoteAction()`**: Run an action locally or forward it to another ESP using a one-time nonce and a signed request.

### Endpoints

With `useLogin = true` all dashboard endpoints require HTTP Basic Auth. Write requests also require `Content-Type: application/json` and an own or empty `Origin` (CSRF). Requests with a foreign `Host` header get 403 (DNS rebinding). A failed login blocks the client IP for 1 s (429).

| Version | Endpoint | Purpose |
|---------|----------|---------|
| Single | `GET /` | Web UI |
| Single | `GET /status` | `{online, reachable, power}`, `power` is `null` if `usePowerSense` is false |
| Single | `POST /poweron` | Press the power button, 409 if the server is already running |
| Single | `GET /api/diag` | Diagnostics |
| Multi | `GET /` | Dashboard |
| Multi | `GET /api/status` | Status of all servers (`getStatusJson()`) |
| Multi | `POST /control` | `{"target":"local"\|<id>,"action":"power"\|"shutdown"\|"reset"}`, 409 if the action does not fit the server state, 501 without reset pin |
| Multi | `GET /api/wstoken` | `{"token"}` for the WebSocket, `no-store` |
| Multi | `GET /api/diag` | Diagnostics |
| Multi | WebSocket `ws://<esp>:81/?t=<token>` | Pushes `{"type":"update","servers":{...}}`, checks token and origin |
| Multi (swarm) | `GET /api/nonce` | One-time nonce, valid 30 s |
| Multi (swarm) | `GET /api/localstatus?c=<challenge>` | Signed status payload |
| Multi (swarm) | `POST /api/control` | `{action, nonce, sig}` from another ESP |

The swarm endpoints need no login. They are protected by an HMAC-SHA256 over the shared `SWARM_KEY` and answer 403 when the swarm is disabled.

### Diagnostics (`/api/diag`, both variants)

Returns `uptime_ms`, `reset_reason` (number from `esp_reset_reason()`, e.g. 1 power-on, 3 software, 4 panic, 5 interrupt watchdog, 6 task watchdog, 9 brownout), `free_heap`, `min_free_heap`, `rssi`, `wifi_disconnects` (without the sketch's own disconnects, reason 8), `last_disconnect_reason` (WiFi reason code, 0 = none), `max_loop_ms` and `version`. A `loop()` run that hangs forever never shows up in `max_loop_ms`. The watchdog restarts the ESP instead, which then shows as `reset_reason`.

---

## Robustness

- **Task watchdog**: 15 s (`watchdogTimeoutS`). Restarts the ESP if `loop()` (and in the multi-node variant the `monitorTask`) hangs.
- **WiFi**: Auto-reconnect plus a check every 10 s, a restart after 5 minutes without WiFi (`wifiRestartTimeout`), and `setup()` does not wait longer than 20 s for the first connection. WiFi events are counted and logged on the serial console.
- **Option `reduceTxPower`**: Lowers the WiFi transmit power to 8.5 dBm (default on), useful for boards with weak antennas.

## Security

- Optional login (HTTP Basic Auth, only with `useLogin = true`, default off) with credentials from `secrets.h`. The default password `serverwatch` triggers a serial warning in both variants and a hint in the multi-node dashboard.
- CSRF protection, host header check against DNS rebinding, login throttling per IP.
- WebSocket requires a token and checks the origin.
- Swarm traffic is signed (HMAC-SHA256) with challenge-response for status and one-time nonces for commands.
- HTTP is not encrypted. Run the devices only in trusted networks and use a VPN for remote access.

## Testing

`test/run_tests.sh` runs the whole suite: host simulation of both sketches (single, swarm, robustness, security, dashboard in jsdom) and a real PlatformIO firmware build for the ESP32-C3 (`test/firmware/build.sh`). The GitHub workflow `.github/workflows/tests.yml` runs the simulation and the firmware build as two jobs on every push and pull request. See [test/README.md](test/README.md).
