# Project Overview & Purpose

**ServerWatch** is an ESP32/ESP32-C3 based server monitoring and remote control system. It continuously monitors a local server's network availability (via ping/TCP connect) and power status (via mainboard voltage detection on a GPIO pin). It also allows remote power button triggering (and optionally reset button triggering) through GPIO pins.

The repository provides two variants:
1. **Single-Node (`Serverwatch.ino`)**: A simple, standalone HTTP web-server version that monitors a single server and provides a basic web UI.
2. **Multi-Node (`ServerWatch_Multi.ino`)**: An advanced setup that uses WebSocket updates, mDNS auto-discovery, and JSON-based communication to discover other ServerWatch ESPs on the network and show a consolidated multi-server dashboard.

---

## Tech Stack

- **Microcontroller**: ESP32 / ESP32-C3
- **Languages**: C++ (Arduino Framework), HTML/CSS/JavaScript (Frontend UI)
- **Core Libraries**:
  - `WiFi.h` (ESP32 WiFi)
  - `WebServer.h` (HTTP server)
  - `WiFiClient.h` (TCP sockets for connectivity checking)
  - `ESPmDNS.h` (mDNS Service Discovery for Multi-Node variant)
  - `ArduinoJson.h` (JSON serialization/deserialization)
  - `WebSocketsServer.h` (WebSocket server for real-time UI updates)
  - `HTTPClient.h` (HTTP requests to remote nodes)

---

## Folder Structure & Key Files

```
ServerWatch/
├── Serverwatch.ino                # Standalone single-node version (HTTP API + UI)
├── ServerWatch_Multi.ino          # Advanced multi-node version (WebSockets + mDNS)
├── serverwatch_Multi_interface.html # Raw external interface HTML for development/testing
├── README.md                      # General user guide & wiring documentation
├── LICENSE                        # MIT License
└── docs/                          # Project documentation and screenshots
    └── screenshots/
```

- **[Serverwatch.ino](file:///home/alex/Dokumente/Development/ServerWatch/Serverwatch.ino)**: Entry point and source code for the standalone monitoring system.
- **[ServerWatch_Multi.ino](file:///home/alex/Dokumente/Development/ServerWatch/ServerWatch_Multi.ino)**: Entry point and source code for the multi-node monitoring system with WebSockets.
- **[serverwatch_Multi_interface.html](file:///home/alex/Dokumente/Development/ServerWatch/serverwatch_Multi_interface.html)**: Clean HTML interface code (matches the inline template in the multi-node source code).

---

## Main Entry Points & Core Logic

### 1. `Serverwatch.ino`
- **Setup (`setup()`)**: Initializes GPIOs (Power Check & Power Button), connects to Wi-Fi, registers HTTP route handlers (`/`, `/status`, `/poweron`), and starts the web server.
- **Loop (`loop()`)**: Handles web client requests and periodically (every 3 seconds) checks server power state and TCP reachability.
- **Reachability Check (`checkServerReachable()`)**: Attempts to open a TCP connection to the specified server IP and port.

### 2. `ServerWatch_Multi.ino`
- **Setup (`setup()`)**: Sets up GPIOs, establishes WiFi connection, configures mDNS, and starts both the HTTP web server and WebSocket server.
- **Loop (`loop()`)**: Handles HTTP requests, keeps the WebSocket loop alive, checks local server status (every 3 seconds) sending updates to WebSocket clients, and performs an mDNS query (every 10 seconds) to auto-discover and query other ESP nodes.
- **Discovery (`scanForESPs()`)**: Uses `MDNS.queryService` to locate other ESPs, fetches their local status using `/api/localstatus`, and populates a map of remote nodes.
