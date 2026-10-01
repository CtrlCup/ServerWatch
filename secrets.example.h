// Vorlage für die Zugangsdaten: nach "secrets.h" kopieren (liegt neben den .ino-Dateien)
// und anpassen. secrets.h steht in der .gitignore und wird nie committet.
#pragma once

// WLAN
#define WIFI_SSID "DEIN_WLAN_NAME"
#define WIFI_PASSWORD "DEIN_WLAN_PASSWORT"

// Login für das Web-Interface (HTTP Basic Auth). Ohne eigenes Passwort zeigt das
// Dashboard eine Warnung an. Werden nur genutzt, wenn useLogin = true im Sketch gesetzt ist.
#define WEB_USER "admin"
#define WEB_PASSWORD "bitte-aendern"

// Nur Multi-Version: gemeinsamer Schlüssel aller ESPs im Schwarm (mind. 16 Zeichen,
// auf allen ESPs identisch). Ohne Schlüssel bleibt der Schwarm deaktiviert.
#define SWARM_KEY "hier-einen-langen-zufaelligen-schluessel-eintragen"
