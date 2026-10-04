// Vorlage für die Zugangsdaten: nach "secrets.h" kopieren (liegt neben ServerTemp.ino)
// und anpassen. secrets.h steht in der .gitignore und wird nie committet.
#pragma once

// WLAN
#define WIFI_SSID "DEIN_WLAN_NAME"
#define WIFI_PASSWORD "DEIN_WLAN_PASSWORT"

// Login für das Web-Interface (HTTP Basic Auth). Wird nur genutzt, wenn useLogin = true
// im Sketch gesetzt ist (für Racks empfohlen: ohne Login kann jedes Gerät im LAN die Kühlung abschalten).
#define WEB_USER "admin"
#define WEB_PASSWORD "bitte-aendern"
