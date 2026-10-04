#include <WiFi.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <ArduinoJson.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include <Preferences.h>
#include <esp_task_wdt.h>
#include <esp_idf_version.h>
#include <esp_arduino_version.h>

// ========================================
// KONFIGURATIONSVARIABLEN - BITTE ANPASSEN
// ========================================

// Zugangsdaten stehen in secrets.h (Vorlage: secrets.example.h), nicht im Sketch
#if __has_include("secrets.h")
#include "secrets.h"
#endif
#ifndef WIFI_SSID
#define WIFI_SSID "DEIN_WLAN_NAME"
#endif
#ifndef WIFI_PASSWORD
#define WIFI_PASSWORD "DEIN_WLAN_PASSWORT"
#endif
#ifndef WEB_USER
#define WEB_USER "admin"
#endif
#ifndef WEB_PASSWORD
#define WEB_PASSWORD "servertemp"                 // Standard-Passwort: bitte in secrets.h ändern
#endif
const char* ssid = WIFI_SSID;
const char* password = WIFI_PASSWORD;
const char* webUser = WEB_USER;
const char* webPassword = WEB_PASSWORD;

// Gerät
const char* deviceName = "Rack-Lüfter";           // Name des Geräts (Teil des Hostnamens)
const char* fanType = "pwm4";                     // "pwm4" = 4-Pin-Lüfter mit Tacho, "dc3" = 3-Pin-Lüfter über MOSFET

// GPIO Pin Konfiguration
const int ONE_WIRE_PIN = 3;                       // DS18B20 (Datenleitung mit 4,7 kOhm Pull-up)
const int FAN_PWM_PIN = 5;                        // PWM-Ausgang (4-Pin: PWM-Leitung, 3-Pin: MOSFET-Gate)
const int FAN_TACH_PIN = 6;                       // Tacho-Eingang
const int FAN_POWER_PIN = -1;                     // optional: MOSFET/Relais für echtes Aus bei pwm4 (-1 = nicht verwendet)
const bool pwmInverted = false;                   // true, wenn der PWM-Ausgang invertiert wirkt

// Regelung
const int emergencyTemp = 60;                     // ab dieser Temperatur (°C) immer 100 %, Ende unter emergencyTemp - 5
const int fanMinPct = 20;                         // dc3: kleinere Werte werden zu 0 (der Lüfter läuft sonst nicht an)
const unsigned long kickstartMs = 1000;           // dc3: Anlauf aus dem Stand mit 100 % für so lange

// Netzwerk
const bool reduceTxPower = true;                  // true senkt die WLAN-Sendeleistung auf 8,5 dBm (hilft z. B. beim ESP32-C3 Super Mini mit schwacher Antenne); false für volle Sendeleistung
const bool useLogin = false;                      // true, wenn der ESP außerhalb des privaten Netzes erreichbar ist (aktiviert Login per HTTP Basic Auth)
const char* localDomain = "fritz.box";            // zusätzliche lokale DNS-Domain des Routers, "" = keine

// Timing Konfiguration
const int watchdogTimeoutS = 15;                  // Sekunden bis zum automatischen Neustart bei Hänger
const unsigned long wifiRestartTimeout = 300000;  // Neustart, wenn WLAN so lange (ms) getrennt bleibt
const unsigned long wifiBootTimeout = 20000;      // So lange (ms) wartet setup() auf das WLAN

// ========================================
// ENDE DER KONFIGURATIONSVARIABLEN
// ========================================

const char* firmwareVersion = "1.0.0";

// Typen stehen oben, damit die automatisch erzeugten Funktionsprototypen sie kennen
const int MAX_POINTS = 6;
struct Curve {
    int n;
    int t[MAX_POINTS];                            // Temperatur in °C, streng steigend
    int p[MAX_POINTS];                            // Lüfter in %, nicht fallend
};

enum FanMode : uint8_t { MODE_AUTO = 0, MODE_MANUAL = 1, MODE_OFF = 2 };

// Ergebnis eines Steuerbefehls: HTTP-Code (200 = ausgeführt) und Fehlertext
struct ActionResult {
    int code;
    String error;
};

// Ein in NVS gespeicherter Wert, mit Entprellung (nur bei Änderung, 30 s nach der letzten Änderung, höchstens 1/min)
struct NvsItem {
    const char* key;
    uint8_t want[2 + 2 * MAX_POINTS];             // gewünschter Wert
    uint8_t wantLen;
    uint8_t have[2 + 2 * MAX_POINTS];             // Wert im NVS
    uint8_t haveLen;
    unsigned long changedAt;
    unsigned long lastWrite;
    bool written;
};

const unsigned long nvsQuietMs = 30000;
const unsigned long nvsMinGapMs = 60000;
const char* NVS_NS = "servertemp";

const unsigned long sensorIntervalMs = 2000;      // DS18B20 alle 2 s abfragen
const unsigned long sensorConvMs = 400;           // Auslesen nach 400 ms (11 Bit brauchen 375 ms)
const unsigned long sensorRescanMs = 30000;
const int PWM_CHANNEL = 0;                        // nur Core 2.x
const int PWM_FREQ = 25000;
const int PWM_BITS = 8;

// Globale Variablen
WebServer server(80);
OneWire oneWire(ONE_WIRE_PIN);
DallasTemperature sensors(&oneWire);
Preferences prefs;

String espHostname;
String nodeId;                                    // eindeutige ID dieses ESP (MAC ohne Doppelpunkte)
bool defaultCredentials = false;
bool dc3 = false;                                 // fanType == "dc3"
unsigned long lastWiFiCheck = 0;
const unsigned long wifiCheckInterval = 10000;
bool wifiLost = false;
unsigned long wifiLostSince = 0;
volatile uint32_t wifiDisconnects = 0;
volatile uint8_t lastDisconnectReason = 0;
unsigned long maxLoopMs = 0;

// Kennlinie und Modus
Curve curve = {4, {25, 35, 45, 55}, {30, 45, 70, 100}};
FanMode mode = MODE_AUTO;
int manPct = 50;
NvsItem nvsCurve = {"curve"};
NvsItem nvsMode = {"mode"};
NvsItem nvsManPct = {"manPct"};

// Sensor
enum SensorPhase : uint8_t { SENSOR_IDLE, SENSOR_CONVERTING };
SensorPhase sensorPhase = SENSOR_IDLE;
unsigned long sensorRequestAt = 0;
unsigned long sensorScanAt = 0;
bool sensorOk = true;                             // false nach 3 ungültigen Messungen in Folge
bool lastValid = false;                           // letzte Messung gültig (für den 85-°C-Einschaltwert)
int sensorFails = 0;
bool hasTemp = false;
float tempRaw = 0;                                // letzter Messwert (für Notlauf und Anzeige)
float tempEma = 0;                                // geglättet (für die Kennlinie)
bool overtemp = false;
bool bootHold = true;                             // 100 % bis zur ersten gültigen Messung

// Ausgang
float outPct = 100;                               // aktueller Ausgang in %, nach der Rampe
unsigned long lastControl = 0;
unsigned long kickUntil = 0;
bool offLatched = false;                          // Kennlinie hat auf 0 % geführt: erst bei offTemp + 2 K wieder an
float offTemp = 0;
int lastDuty = -1;

// Tacho
portMUX_TYPE tachMux = portMUX_INITIALIZER_UNLOCKED;
volatile uint32_t tachPulses = 0;
volatile uint32_t tachLastUs = 0;
unsigned long tachAt = 0;
bool rpmValid = false;
uint32_t rpm = 0;
bool stall = false;
unsigned long stallSince = 0;

// Fehlversuche beim Login: 1 s Sperre pro IP
struct FailedLogin {
    uint32_t ip;
    unsigned long at;
};
FailedLogin failedLogins[8] = {};
int failedLoginNext = 0;

// Minimale Weboberfläche: zeigt /api/status (das gemeinsame Dashboard folgt später)
const char htmlPage[] PROGMEM = R"HTML(<!DOCTYPE html>
<html lang="de"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>ServerTemp</title></head>
<body style="font-family:sans-serif;margin:1em"><h1>ServerTemp</h1><pre id="o">Lade ...</pre>
<script>
async function u(){try{const r=await fetch('/api/status');const j=await r.json();
document.getElementById('o').textContent=JSON.stringify(j.servers.local,null,2);}
catch(e){document.getElementById('o').textContent='Keine Verbindung';}}
u();setInterval(u,3000);
</script></body></html>)HTML";

// ================= Lüfter-Ausgang =================

// LEDC einrichten (Core 2.x und 3.x haben unterschiedliche APIs)
void fanPwmBegin() {
#if ESP_ARDUINO_VERSION_MAJOR >= 3
    ledcAttach(FAN_PWM_PIN, PWM_FREQ, PWM_BITS);
#else
    ledcSetup(PWM_CHANNEL, PWM_FREQ, PWM_BITS);
    ledcAttachPin(FAN_PWM_PIN, PWM_CHANNEL);
#endif
}

// Ausgang in % setzen; geschrieben wird nur bei geändertem Duty-Wert
void fanPwmWrite(float pct) {
    int duty = (int)lroundf(pct * 255.0f / 100.0f);
    if (duty < 0) duty = 0;
    if (duty > 255) duty = 255;
    if (duty == lastDuty) return;
    lastDuty = duty;
    if (FAN_POWER_PIN >= 0) digitalWrite(FAN_POWER_PIN, duty > 0 ? HIGH : LOW);
    int out = pwmInverted ? 255 - duty : duty;
#if ESP_ARDUINO_VERSION_MAJOR >= 3
    ledcWrite(FAN_PWM_PIN, out);
#else
    ledcWrite(PWM_CHANNEL, out);
#endif
}

// Tacho-Interrupt: Flanken zählen, Störimpulse unter 1 ms ignorieren
void IRAM_ATTR tachIsr() {
    uint32_t now = micros();
    if (now - tachLastUs < 1000) return;
    tachLastUs = now;
    portENTER_CRITICAL_ISR(&tachMux);
    tachPulses++;
    portEXIT_CRITICAL_ISR(&tachMux);
}

// ================= Kennlinie =================

// Gibt einen Fehlertext zurück oder nullptr, wenn die Kennlinie gültig ist
const char* curveError(const Curve& c) {
    if (c.n < 2 || c.n > MAX_POINTS) return "2 bis 6 Punkte erforderlich";
    for (int i = 0; i < c.n; i++) {
        if (c.t[i] < -10 || c.t[i] > 100) return "Temperatur muss zwischen -10 und 100 liegen";
        if (c.p[i] < 0 || c.p[i] > 100) return "Prozentwert muss zwischen 0 und 100 liegen";
        if (i > 0 && c.t[i] <= c.t[i - 1]) return "Temperaturen müssen streng steigen";
        if (i > 0 && c.p[i] < c.p[i - 1]) return "Prozentwerte dürfen nicht fallen";
    }
    return nullptr;
}

// Linear interpoliert, außerhalb der Punkte konstant
float curvePct(const Curve& c, float temp) {
    if (temp <= c.t[0]) return c.p[0];
    for (int i = 1; i < c.n; i++) {
        if (temp <= c.t[i]) {
            float f = (temp - c.t[i - 1]) / (float)(c.t[i] - c.t[i - 1]);
            return c.p[i - 1] + f * (c.p[i] - c.p[i - 1]);
        }
    }
    return c.p[c.n - 1];
}

// ================= NVS =================

void nvsStage(NvsItem& item, const uint8_t* data, uint8_t len) {
    if (len == item.wantLen && memcmp(item.want, data, len) == 0) return;
    memcpy(item.want, data, len);
    item.wantLen = len;
    item.changedAt = millis();
}

// Beginn des Zustands, den NVS schon enthält (oder der Standard): kein Schreibvorgang nötig
void nvsInit(NvsItem& item, const uint8_t* data, uint8_t len) {
    memcpy(item.want, data, len);
    memcpy(item.have, data, len);
    item.wantLen = item.haveLen = len;
}

void encodeCurve(const Curve& c, uint8_t* out) {
    out[0] = 1;  // Version
    out[1] = c.n;
    for (int i = 0; i < c.n; i++) {
        out[2 + 2 * i] = (uint8_t)(int8_t)c.t[i];
        out[3 + 2 * i] = (uint8_t)c.p[i];
    }
}

void stageCurve() {
    uint8_t buf[2 + 2 * MAX_POINTS];
    encodeCurve(curve, buf);
    nvsStage(nvsCurve, buf, 2 + 2 * curve.n);
}

// "Aus" wird nie gespeichert: nach einem Neustart läuft der Lüfter in Auto
void stageMode() {
    uint8_t m = mode == MODE_MANUAL ? 1 : 0;
    nvsStage(nvsMode, &m, 1);
}

void stageManPct() {
    uint8_t m = (uint8_t)manPct;
    nvsStage(nvsManPct, &m, 1);
}

void nvsWriteIfDue(NvsItem& item, bool isBlob) {
    if (item.wantLen == item.haveLen && memcmp(item.want, item.have, item.wantLen) == 0) return;
    unsigned long now = millis();
    if (now - item.changedAt < nvsQuietMs) return;
    if (item.written && now - item.lastWrite < nvsMinGapMs) return;
    prefs.begin(NVS_NS, false);
    if (isBlob) prefs.putBytes(item.key, item.want, item.wantLen);
    else prefs.putUChar(item.key, item.want[0]);
    prefs.end();
    memcpy(item.have, item.want, item.wantLen);
    item.haveLen = item.wantLen;
    item.lastWrite = now;
    item.written = true;
}

void nvsTick() {
    nvsWriteIfDue(nvsCurve, true);
    nvsWriteIfDue(nvsMode, false);
    nvsWriteIfDue(nvsManPct, false);
}

// Gespeicherte Einstellungen laden; ungültige Inhalte ergeben den Standard
void loadSettings() {
    prefs.begin(NVS_NS, false);
    uint8_t buf[2 + 2 * MAX_POINTS];
    size_t len = prefs.getBytesLength("curve");
    if (len >= 2 && len <= sizeof buf && prefs.getBytes("curve", buf, sizeof buf) == len && buf[0] == 1 && len == 2u + 2u * buf[1]) {
        Curve c;
        c.n = buf[1];
        for (int i = 0; i < c.n && i < MAX_POINTS; i++) {
            c.t[i] = (int8_t)buf[2 + 2 * i];
            c.p[i] = buf[3 + 2 * i];
        }
        if (curveError(c) == nullptr) curve = c;
    }
    uint8_t m = prefs.getUChar("mode", 0);
    if (m == 1) mode = MODE_MANUAL;
    uint8_t mp = prefs.getUChar("manPct", 50);
    if (mp <= 100) manPct = mp;
    prefs.end();

    encodeCurve(curve, buf);
    nvsInit(nvsCurve, buf, 2 + 2 * curve.n);
    uint8_t mv = mode == MODE_MANUAL ? 1 : 0;
    nvsInit(nvsMode, &mv, 1);
    uint8_t pv = (uint8_t)manPct;
    nvsInit(nvsManPct, &pv, 1);
}

// ================= Sensor =================

void sensorInit() {
    sensors.begin();
    sensors.setResolution(11);
    sensors.setWaitForConversion(false);
}

bool validTemp(float v) {
    if (v == DEVICE_DISCONNECTED_C || v < -20.0f || v > 125.0f) return false;
    if (v == 85.0f && !lastValid) return false;  // Einschaltwert des DS18B20
    return true;
}

// Nicht blockierend: alle 2 s eine Konversion anstoßen, nach 400 ms auslesen
void sensorTick() {
    unsigned long now = millis();
    if (sensorPhase == SENSOR_IDLE) {
        if (now - sensorRequestAt < sensorIntervalMs) return;
        if (!sensorOk && now - sensorScanAt >= sensorRescanMs) {
            sensorInit();  // Bus neu absuchen
            sensorScanAt = now;
        }
        sensors.requestTemperatures();
        sensorRequestAt = now;
        sensorPhase = SENSOR_CONVERTING;
        return;
    }
    if (now - sensorRequestAt < sensorConvMs) return;
    sensorPhase = SENSOR_IDLE;
    float v = sensors.getTempCByIndex(0);
    if (validTemp(v)) {
        lastValid = true;
        sensorFails = 0;
        sensorOk = true;
        bootHold = false;
        tempRaw = v;
        if (!hasTemp) tempEma = v;
        else tempEma += 0.3f * (v - tempEma);
        hasTemp = true;
        // Notlauf: Ende erst 5 K unter der Schwelle
        if (v >= emergencyTemp) overtemp = true;
        else if (v < emergencyTemp - 5) overtemp = false;
    } else {
        lastValid = false;
        if (sensorFails < 100) sensorFails++;
        if (sensorFails >= 3 && sensorOk) {
            sensorOk = false;
            hasTemp = false;
            Serial.println("Sensorfehler: DS18B20 liefert keine gültigen Werte");
        }
    }
}

// ================= Tacho =================

// Jede Sekunde: Pulse auswerten, rpm = Pulse pro Sekunde * 30 (2 Pulse je Umdrehung)
void tachTick() {
    unsigned long now = millis();
    if (now - tachAt < 1000) return;
    unsigned long dt = now - tachAt;
    tachAt = now;
    portENTER_CRITICAL(&tachMux);
    uint32_t pulses = tachPulses;
    tachPulses = 0;
    portEXIT_CRITICAL(&tachMux);
    uint32_t raw = pulses * 30000UL / dt;
    rpm = (raw == 0 || !rpmValid || rpm == 0) ? raw : (rpm + raw) / 2;
    rpmValid = true;
    // Stillstand: nur pwm4, 5 s lang bei mindestens 30 %
    if (!dc3 && outPct >= 30 && raw == 0) {
        if (stallSince == 0) stallSince = now | 1;
        if (now - stallSince >= 5000) stall = true;
    } else {
        stallSince = 0;
        stall = false;
    }
}

// ================= Regelung =================

const char* failsafeName() {
    if (overtemp) return "overtemp";
    if (!sensorOk) return "sensor";
    return "";
}

// Alle 100 ms: Zielwert bestimmen und den Ausgang per Rampe (+10 %/s, -2 %/s) nachführen
void controlTick() {
    unsigned long now = millis();
    unsigned long dt = now - lastControl;
    if (dt < 100) return;
    lastControl = now;

    float target;
    bool immediate = true;  // Notlauf, Sensorfehler, Boot und "Aus" wirken ohne Rampe
    bool failsafe = false;
    if (overtemp) {
        target = 100;
        failsafe = true;
    } else if (mode == MODE_OFF) {
        target = 0;
    } else if (!sensorOk || bootHold) {
        target = 100;
        failsafe = true;
    } else if (mode == MODE_MANUAL) {
        target = manPct;
        immediate = false;
    } else {
        float t = curvePct(curve, tempEma);
        if (offLatched && !(t > 0 && tempEma >= offTemp + 2)) t = 0;
        else offLatched = false;
        target = lroundf(t);  // ganze Prozent, wie im Status
        immediate = false;
    }
    if (failsafe || mode != MODE_AUTO) offLatched = false;
    if (dc3 && target < fanMinPct) target = 0;

    if (immediate) {
        outPct = target;
        kickUntil = 0;
    } else if (dc3 && outPct == 0 && target > 0 && kickUntil == 0) {
        kickUntil = now + kickstartMs;  // Anlauf aus dem Stand: erst voll aufdrehen
        outPct = 100;
    } else if (kickUntil != 0 && (long)(now - kickUntil) < 0 && target > 0) {
        outPct = 100;
    } else {
        kickUntil = 0;
        float seconds = dt / 1000.0f;
        if (outPct < target) outPct = fminf(target, outPct + 10.0f * seconds);
        else if (outPct > target) outPct = fmaxf(target, outPct - 2.0f * seconds);
        // Beim Herunterrampen auf 0: unter 1 % (dc3: unter fanMinPct) gleich abschalten, kein Nachziehen im Bereich 0 bis 1 %
        if (target == 0 && outPct < (dc3 ? (float)fanMinPct : 1.0f)) outPct = 0;
    }

    // Kennlinie 0 %: Ausschalttemperatur merken, Wiedereinschalten erst bei +2 K
    if (!immediate && mode == MODE_AUTO && outPct == 0 && curvePct(curve, tempEma) <= 0 && !offLatched) {
        offLatched = true;
        offTemp = tempEma;
    }
    fanPwmWrite(outPct);
}

// Aktion prüfen und ausführen: fan:auto, fan:manual, fan:off, fan:speed:N (N = 0 bis 100)
ActionResult executeFanAction(const String& action) {
    if (action.length() > 16) return {400, "Aktion zu lang"};
    if (action == "fan:auto") {
        mode = MODE_AUTO;
    } else if (action == "fan:manual") {
        manPct = (int)lroundf(outPct);  // aktuellen Wert einfrieren
        mode = MODE_MANUAL;
    } else if (action == "fan:off") {
        mode = MODE_OFF;
    } else if (action.startsWith("fan:speed:")) {
        String digits = action.substring(10);
        if (digits.length() < 1 || digits.length() > 3) return {400, "Ungültige Drehzahl"};
        for (unsigned int i = 0; i < digits.length(); i++) {
            if (digits[i] < '0' || digits[i] > '9') return {400, "Ungültige Drehzahl"};
        }
        if (digits.length() > 1 && digits[0] == '0') return {400, "Ungültige Drehzahl"};
        int value = digits.toInt();
        if (value > 100) return {400, "Drehzahl über 100"};
        manPct = value;
        mode = MODE_MANUAL;
    } else {
        return {400, "Unbekannte Aktion"};
    }
    stageMode();
    stageManPct();
    Serial.println("Aktion ausgeführt: " + action);
    return {200, ""};
}

// ================= Netzwerk und Sicherheit =================

// Hostname nach RFC 1123: nur a-z, 0-9 und '-', Umlaute umschreiben, MAC-Suffix für Eindeutigkeit
String makeHostname(const String& name, const String& id, const String& prefix) {
    String base = name;
    base.toLowerCase();
    base.replace("ä", "ae"); base.replace("ö", "oe"); base.replace("ü", "ue");
    base.replace("Ä", "ae"); base.replace("Ö", "oe"); base.replace("Ü", "ue"); base.replace("ß", "ss");
    String clean;
    for (unsigned int i = 0; i < base.length() && clean.length() < 20; i++) {
        char c = base[i];
        bool ok = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');
        if (ok) clean += c;
        else if (clean.length() > 0 && clean[clean.length() - 1] != '-') clean += '-';
    }
    while (clean.endsWith("-")) clean.remove(clean.length() - 1);
    String host = prefix;
    if (clean.length() > 0) host += clean + "-";
    return host + id.substring(id.length() - 6);
}

// WLAN-Ereignisse mitschreiben. Läuft im Event-Task: nichts Blockierendes, keine WiFi-Aufrufe
void onWiFiEvent(arduino_event_id_t event, arduino_event_info_t info) {
    if (event == ARDUINO_EVENT_WIFI_STA_DISCONNECTED) {
        uint8_t reason = info.wifi_sta_disconnected.reason;
        Serial.printf("WiFi getrennt, Grund %u (%s)\n", reason, WiFi.disconnectReasonName((wifi_err_reason_t)reason));
        // Grund 8 (ASSOC_LEAVE) löst unser eigenes WiFi.disconnect() im Reconnect-Fallback aus: nicht zählen
        if (reason != WIFI_REASON_ASSOC_LEAVE) {
            wifiDisconnects++;
            lastDisconnectReason = reason;
        }
    } else if (event == ARDUINO_EVENT_WIFI_STA_GOT_IP) {
        Serial.println("WiFi: IP erhalten");
    } else if (event == ARDUINO_EVENT_WIFI_STA_LOST_IP) {
        Serial.println("WiFi: IP verloren");
    }
}

// WLAN starten; mit reduceTxPower danach die Sendeleistung senken
void startWiFi() {
    WiFi.begin(ssid, password);
    if (reduceTxPower) WiFi.setTxPower(WIFI_POWER_8_5dBm);
}

// WiFi Setup
void setupWiFi() {
    Serial.println("\n=== ServerTemp ===");
    Serial.print("Verbinde mit WiFi: ");
    Serial.println(ssid);

    // Eindeutiger Hostname aus Gerätename und MAC, z. B. "servertemp-rack-luefter-a1b2c3"
    nodeId = WiFi.macAddress();
    nodeId.replace(":", "");
    nodeId.toLowerCase();
    espHostname = makeHostname(deviceName, nodeId, "servertemp-");
    WiFi.setHostname(espHostname.c_str());

    // Stellt sicher, dass das WiFi-Modul nicht in den Schlafmodus geht (wichtig für Fritzboxen)
    WiFi.setSleep(false);
    WiFi.setAutoReconnect(true);

    WiFi.onEvent(onWiFiEvent);  // vor WiFi.begin() registrieren, damit kein Ereignis fehlt
    startWiFi();
    if (reduceTxPower) Serial.println("Sendeleistung reduziert (8,5 dBm)");

    // Nicht endlos warten: klappt es nicht, übernimmt der Reconnect im loop()
    unsigned long wifiStart = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - wifiStart < wifiBootTimeout) {
        delay(500);
        Serial.print(".");
    }

    if (WiFi.status() == WL_CONNECTED) {
        Serial.println("\nWiFi verbunden!");
        Serial.print("IP-Adresse: ");
        Serial.println(WiFi.localIP());
    } else {
        Serial.println("\nWiFi noch nicht verbunden - neuer Versuch im Hintergrund");
    }
    Serial.print("Hostname: ");
    Serial.println(espHostname);
}

// mDNS: Gerät unter <hostname>.local erreichbar machen
void setupMDNS() {
    if (!MDNS.begin(espHostname.c_str())) {
        Serial.println("Fehler beim mDNS Setup!");
    }
}

// Ist host (ggf. mit Port) eine Adresse dieses ESP? Schutz gegen DNS-Rebinding.
bool isOwnHost(String host) {
    host.toLowerCase();
    int colon = host.indexOf(':');
    if (colon >= 0) host = host.substring(0, colon);
    if (host.length() == 0) return false;
    if (host == WiFi.localIP().toString()) return true;
    String own = espHostname;
    own.toLowerCase();
    if (host == own || host == own + ".local") return true;
    String domain = localDomain;
    domain.toLowerCase();
    return domain.length() > 0 && host == own + "." + domain;
}

// Origin leer (kein Browser) oder eigene Adresse
bool isAllowedOrigin(const String& origin) {
    if (origin.length() == 0) return true;
    if (!origin.startsWith("http://")) return false;  // auch "null"
    String host = origin.substring(7);
    if (host.indexOf('/') >= 0) return false;
    return isOwnHost(host);
}

void sendError(int code, const char* message) {
    DynamicJsonDocument doc(256);
    doc["success"] = false;
    doc["error"] = message;
    String output;
    serializeJson(doc, output);
    server.send(code, "application/json", output);
}

// Jede Anfrage: Host-Header muss zu diesem ESP gehören
bool checkHost() {
    if (isOwnHost(server.hostHeader())) return true;
    sendError(403, "Unbekannter Host");
    return false;
}

bool loginThrottled(uint32_t ip) {
    for (const FailedLogin& f : failedLogins) {
        if (f.ip == ip && f.at != 0 && millis() - f.at < 1000) return true;
    }
    return false;
}

// Host prüfen und Login verlangen (HTTP Basic Auth)
bool requireAuth() {
    if (!checkHost()) return false;
    if (!useLogin) return true;
    uint32_t ip = (uint32_t)server.client().remoteIP();
    if (loginThrottled(ip)) {
        sendError(429, "Zu viele Fehlversuche, bitte kurz warten");
        return false;
    }
    if (server.authenticate(webUser, webPassword)) return true;
    if (server.hasHeader("Authorization")) {
        failedLogins[failedLoginNext] = {ip, millis() | 1};
        failedLoginNext = (failedLoginNext + 1) % 8;
    }
    server.requestAuthentication(BASIC_AUTH, "ServerTemp");
    return false;
}

// Schreibende Anfragen: nur JSON und nur von der eigenen Seite (CSRF)
bool checkJsonWrite() {
    String contentType = server.header("Content-Type");
    contentType.toLowerCase();
    if (!contentType.startsWith("application/json")) {
        sendError(415, "Content-Type application/json erforderlich");
        return false;
    }
    if (!isAllowedOrigin(server.header("Origin"))) {
        sendError(403, "Fremder Origin");
        return false;
    }
    return true;
}

// JSON-Body eines Requests lesen; bei Fehler wird direkt 400 gesendet
bool readJsonBody(DynamicJsonDocument& doc) {
    if (!server.hasArg("plain") || deserializeJson(doc, server.arg("plain")) != DeserializationError::Ok || !doc.is<JsonObject>() ||
        (doc.containsKey("action") && !doc["action"].is<const char*>()) || (doc.containsKey("target") && !doc["target"].is<const char*>())) {
        sendError(400, "Ungültiges JSON");
        return false;
    }
    return true;
}

void sendActionResult(const ActionResult& result) {
    if (result.code == 200) server.send(200, "application/json", "{\"success\":true}");
    else sendError(result.code, result.error.c_str());
}

// ================= Web Server =================

String getStatusJson() {
    DynamicJsonDocument doc(1024);
    doc["defaultCredentials"] = defaultCredentials;
    JsonObject servers = doc.createNestedObject("servers");
    JsonObject local = servers.createNestedObject("local");
    local["isLocal"] = true;
    local["type"] = "fan";
    local["id"] = nodeId;
    local["hostname"] = espHostname;
    local["serverName"] = deviceName;
    if (sensorOk && hasTemp) local["temp"] = (double)roundf(tempRaw * 10.0f) / 10.0;
    else local["temp"] = nullptr;
    local["sensorOk"] = sensorOk;
    bool hasRpm = !dc3 || outPct >= 100;  // bei dc3 nur bei 100 % gültig
    if (hasRpm && rpmValid) local["rpm"] = rpm;
    else local["rpm"] = nullptr;
    int pct = (int)lroundf(outPct);
    local["pct"] = pct;
    local["mode"] = mode == MODE_AUTO ? "auto" : (mode == MODE_MANUAL ? "manual" : "off");
    local["fanOn"] = pct > 0 || (hasRpm && rpmValid && rpm > 0);
    local["failsafe"] = failsafeName();
    local["stall"] = stall;
    local["fanType"] = dc3 ? "dc3" : "pwm4";
    local["hasRpm"] = hasRpm;
    local["version"] = firmwareVersion;
    local["espIP"] = WiFi.localIP().toString();
    local["espReachable"] = true;
    String output;
    serializeJson(doc, output);
    return output;
}

String getCurveJson() {
    DynamicJsonDocument doc(1024);
    JsonArray points = doc.createNestedArray("points");
    for (int i = 0; i < curve.n; i++) {
        JsonArray pair = points.createNestedArray();
        pair.add(curve.t[i]);
        pair.add(curve.p[i]);
    }
    doc["emergencyTemp"] = emergencyTemp;
    doc["minPct"] = fanMinPct;
    String output;
    serializeJson(doc, output);
    return output;
}

// POST /api/curve: {"points":[[t,p],...]} streng prüfen, erst dann übernehmen
void handleCurvePost() {
    if (!requireAuth()) return;
    String body = server.arg("plain");
    if (body.length() == 0 && server.header("Content-Type").length() == 0) return sendError(400, "Leerer Body");
    if (!checkJsonWrite()) return;
    if (body.length() > 1024) return sendError(413, "Anfrage zu groß");
    DynamicJsonDocument doc(2048);
    if (deserializeJson(doc, body) != DeserializationError::Ok || !doc.is<JsonObject>()) return sendError(400, "Ungültiges JSON");
    if (!doc["points"].is<JsonArray>()) return sendError(400, "points muss ein Array sein");
    JsonArray points = doc["points"].as<JsonArray>();
    if (points.size() < 2 || points.size() > MAX_POINTS) return sendError(400, "2 bis 6 Punkte erforderlich");
    Curve c;
    c.n = 0;
    for (JsonVariant point : points) {
        if (!point.is<JsonArray>() || point.size() != 2) return sendError(400, "Jeder Punkt muss [Temperatur, Prozent] sein");
        JsonArray pair = point.as<JsonArray>();
        if (!pair[0].is<int>() || !pair[1].is<int>()) return sendError(400, "Punkte müssen ganze Zahlen sein");
        c.t[c.n] = pair[0].as<int>();
        c.p[c.n] = pair[1].as<int>();
        c.n++;
    }
    const char* error = curveError(c);
    if (error) return sendError(400, error);
    curve = c;
    offLatched = false;
    stageCurve();
    server.send(200, "application/json", "{\"success\":true}");
}

void setupWebServer() {
    server.on("/", []() {
        if (!requireAuth()) return;
        server.send_P(200, "text/html", htmlPage);
    });

    server.on("/api/status", []() {
        if (!requireAuth()) return;
        server.send(200, "application/json", getStatusJson());
    });

    server.on("/api/curve", HTTP_GET, []() {
        if (!requireAuth()) return;
        server.send(200, "application/json", getCurveJson());
    });

    server.on("/api/curve", HTTP_POST, handleCurvePost);

    // Diagnose für Feldfehler wie "ESP nicht mehr erreichbar"
    server.on("/api/diag", HTTP_GET, []() {
        if (!requireAuth()) return;
        DynamicJsonDocument doc(512);
        doc["uptime_ms"] = millis();
        doc["reset_reason"] = (int)esp_reset_reason();
        doc["free_heap"] = ESP.getFreeHeap();
        doc["min_free_heap"] = ESP.getMinFreeHeap();
        doc["rssi"] = WiFi.RSSI();
        doc["wifi_disconnects"] = wifiDisconnects;
        doc["last_disconnect_reason"] = lastDisconnectReason;
        doc["max_loop_ms"] = maxLoopMs;
        doc["version"] = firmwareVersion;
        String json;
        serializeJson(doc, json);
        server.sendHeader("Cache-Control", "no-store");
        server.send(200, "application/json", json);
    });

    server.on("/control", HTTP_POST, []() {
        if (!requireAuth() || !checkJsonWrite()) return;
        DynamicJsonDocument doc(512);
        if (!readJsonBody(doc)) return;
        String target = doc["target"] | "";
        String action = doc["action"] | "";
        if (target.length() == 0) sendActionResult({400, "Kein Ziel angegeben"});
        else if (target != "local") sendActionResult({404, "Unbekanntes Ziel"});
        else sendActionResult(executeFanAction(action));
    });

    const char* headerKeys[] = {"Content-Type", "Origin"};
    server.collectHeaders(headerKeys, 2);
    server.begin();
    Serial.println("Web Server gestartet auf Port 80");
}

// Watchdog: startet den ESP neu, wenn loop() hängt
void setupWatchdog() {
#if ESP_IDF_VERSION_MAJOR >= 5
    esp_task_wdt_config_t config = { .timeout_ms = watchdogTimeoutS * 1000, .idle_core_mask = 0, .trigger_panic = true };
    esp_task_wdt_reconfigure(&config);
#else
    esp_task_wdt_init(watchdogTimeoutS, true);
#endif
    enableLoopWDT();
}

// Setup
void setup() {
    Serial.begin(115200);

    // Sicherheit zuerst: Lüfter sofort mit 100 %, bis die erste Messung vorliegt
    dc3 = strcmp(fanType, "dc3") == 0;
    if (FAN_POWER_PIN >= 0) pinMode(FAN_POWER_PIN, OUTPUT);
    fanPwmBegin();
    fanPwmWrite(100);
    pinMode(FAN_TACH_PIN, INPUT_PULLUP);
    attachInterrupt(digitalPinToInterrupt(FAN_TACH_PIN), tachIsr, FALLING);

    defaultCredentials = useLogin && strcmp(webPassword, "servertemp") == 0;
    if (defaultCredentials) Serial.println("WARNUNG: Standard-Passwort aktiv - WEB_PASSWORD in secrets.h setzen");

    loadSettings();
    sensorInit();
    sensorScanAt = sensorRequestAt = tachAt = lastControl = millis();
    sensorRequestAt -= sensorIntervalMs;  // erste Messung sofort

    setupWiFi();
    setupMDNS();
    setupWebServer();
    setupWatchdog();

    Serial.println("\n=== ServerTemp bereit ===");
    Serial.println("Web Interface: http://" + WiFi.localIP().toString());
}

// Main Loop
void loop() {
    unsigned long loopStart = millis();  // Dauer dieses Durchlaufs für /api/diag
    server.handleClient();
    sensorTick();
    tachTick();
    controlTick();
    nvsTick();

    unsigned long currentMillis = millis();

    // WiFi Verbindung prüfen und ggf. neu verbinden; die Regelung läuft unabhängig davon weiter
    if (currentMillis - lastWiFiCheck >= wifiCheckInterval) {
        lastWiFiCheck = currentMillis;
        if (WiFi.status() != WL_CONNECTED) {
            if (!wifiLost) {
                wifiLost = true;
                wifiLostSince = currentMillis;
            } else if (currentMillis - wifiLostSince >= wifiRestartTimeout) {
                Serial.println("WiFi seit langem getrennt - Neustart");
                ESP.restart();
            }
            Serial.println("WiFi Verbindung verloren! Versuche neu zu verbinden...");
            WiFi.disconnect();
            startWiFi();
        } else {
            wifiLost = false;
        }
    }

    unsigned long loopMs = millis() - loopStart;
    if (loopMs > maxLoopMs) maxLoopMs = loopMs;
    delay(2);
}
