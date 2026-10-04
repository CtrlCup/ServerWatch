// Selbsttest der Simulations-Fakes (OneWire/DallasTemperature, LEDC, Interrupts, Preferences).
// Kein Produkt-Sketch: wird nur von test/tests/test_sim_fakes.py gebaut. Ausgabe als key=value-Zeilen.
#include <WiFi.h>
#include <WebServer.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include <Preferences.h>
#include "esp_arduino_version.h"

const int ONE_WIRE_PIN = 3;
const int FAN_PWM_PIN = 5;
const int FAN_TACH_PIN = 6;

WebServer server(80);
OneWire oneWire(ONE_WIRE_PIN);
DallasTemperature sensors(&oneWire);
Preferences prefs;

static portMUX_TYPE tachMux = portMUX_INITIALIZER_UNLOCKED;
volatile uint32_t tachPulses = 0;

void IRAM_ATTR tachIsr() {
  portENTER_CRITICAL_ISR(&tachMux);
  tachPulses++;
  portEXIT_CRITICAL_ISR(&tachMux);
}

static uint32_t readPulses() {
  portENTER_CRITICAL(&tachMux);
  uint32_t v = tachPulses;
  portEXIT_CRITICAL(&tachMux);
  return v;
}

static void line(String &out, const char *key, const String &value) {
  out += key;
  out += "=";
  out += value;
  out += "\n";
}

// Temperatur: Bus neu scannen, nicht blockierend (11 Bit) und blockierend messen.
void handleTemp() {
  String out;
  sensors.begin();
  sensors.setResolution(11);
  line(out, "devs", String((int)sensors.getDeviceCount()));
  sensors.setWaitForConversion(false);
  unsigned long t0 = millis();
  sensors.requestTemperatures();
  line(out, "request_ms", String((long)(millis() - t0)));
  line(out, "complete_immediately", sensors.isConversionComplete() ? "1" : "0");
  while (!sensors.isConversionComplete()) delay(1);
  line(out, "conv_ms", String((long)(millis() - t0)));
  line(out, "temp", String(sensors.getTempCByIndex(0), 2));
  sensors.setWaitForConversion(true);
  t0 = millis();
  sensors.requestTemperatures();
  line(out, "blocking_ms", String((long)(millis() - t0)));
  line(out, "temp_blocking", String(sensors.getTempCByIndex(0), 2));
  line(out, "disconnected_const", String((int)DEVICE_DISCONNECTED_C));
  server.send(200, "text/plain", out);
}

// Allgemeiner Zustand: Version, NVS-Inhalt (Standardwerte -1/none zeigen "nicht vorhanden/ungueltig").
void handleState() {
  String out;
  line(out, "esp", String(ESP_ARDUINO_VERSION_MAJOR) + "." + String(ESP_ARDUINO_VERSION_MINOR) + "." + String(ESP_ARDUINO_VERSION_PATCH));
  line(out, "ver_ok", (ESP_ARDUINO_VERSION >= ESP_ARDUINO_VERSION_VAL(2, 0, 0) && ESP_ARDUINO_VERSION < ESP_ARDUINO_VERSION_VAL(3, 0, 0)) ? "1" : "0");
  line(out, "ver_val", String((long)ESP_ARDUINO_VERSION));
  prefs.begin("selftest");
  line(out, "boots", String((unsigned long)prefs.getUInt("boots", 0)));
  line(out, "uc", String((int)prefs.getUChar("uc", 99)));
  line(out, "i", String((long)prefs.getInt("i", -1)));
  line(out, "b", prefs.getBool("b", false) ? "1" : "0");
  line(out, "s", prefs.getString("s", "none"));
  line(out, "blob_len", String((unsigned long)prefs.getBytesLength("blob")));
  prefs.end();
  server.send(200, "text/plain", out);
}

// Preferences-API einmal durchgehen (schreibt: je Aufruf ein nvs_write pro put...).
void handleNvs() {
  String out;
  Preferences p;
  line(out, "ro_missing_ns", p.begin("no_such_ns", true) ? "1" : "0");
  p.begin("selftest");
  line(out, "put_uc", String((int)p.putUChar("uc", 200)));
  line(out, "put_i", String((int)p.putInt("i", -12345)));
  line(out, "put_b", String((int)p.putBool("b", true)));
  line(out, "put_s", String((int)p.putString("s", "hello")));
  uint8_t blob[4] = {1, 2, 3, 250};
  line(out, "put_blob", String((int)p.putBytes("blob", blob, sizeof blob)));
  line(out, "blob_len", String((int)p.getBytesLength("blob")));
  uint8_t small[2], big[8] = {0};
  line(out, "get_blob_small", String((int)p.getBytes("blob", small, sizeof small)));
  int bigLen = (int)p.getBytes("blob", big, sizeof big);
  line(out, "get_blob_big", String(bigLen) + ":" + String((int)big[3]));
  line(out, "missing_default", String((int)p.getInt("nope", 77)));
  line(out, "is_key", p.isKey("uc") ? "1" : "0");
  line(out, "is_key_missing", p.isKey("nope") ? "1" : "0");
  line(out, "remove_missing", p.remove("nope") ? "1" : "0");
  p.putUChar("tmp", 1);
  line(out, "remove_tmp", p.remove("tmp") ? "1" : "0");
  line(out, "is_key_tmp", p.isKey("tmp") ? "1" : "0");
  line(out, "long_key", String((int)p.putUChar("0123456789abcdef", 1)));
  p.end();
  line(out, "put_after_end", String((int)p.putUChar("uc", 1)));
  Preferences ro;
  ro.begin("selftest", true);
  line(out, "put_readonly", String((int)ro.putUChar("uc", 1)));
  ro.end();
  server.send(200, "text/plain", out);
}

void handleClear() {
  Preferences p;
  p.begin("selftest");
  bool ok = p.clear();
  p.end();
  server.send(200, "text/plain", ok ? "cleared=1\n" : "cleared=0\n");
}

// Tacho: Pulse in ms simulierter Zeit zaehlen; dazu pruefen, dass ein kritischer Abschnitt die ISR blockiert.
void handlePulses() {
  String out;
  long ms = server.hasArg("ms") ? server.arg("ms").toInt() : 1000;
  uint32_t start = readPulses();
  delay(ms);
  line(out, "pulses", String((unsigned long)(readPulses() - start)));
  line(out, "window_ms", String(ms));
  portENTER_CRITICAL(&tachMux);
  uint32_t a = tachPulses;
  delay(300);
  uint32_t b = tachPulses;
  portEXIT_CRITICAL(&tachMux);
  line(out, "crit_delta", String((unsigned long)(b - a)));
  uint32_t c0 = readPulses();
  delay(30);
  line(out, "burst_delta", String((unsigned long)(readPulses() - c0)));
  delay(170);
  line(out, "after_crit_delta", String((unsigned long)(tachPulses - b)));
  noInterrupts();
  a = tachPulses;
  delay(100);
  b = tachPulses;
  interrupts();
  line(out, "nointr_delta", String((unsigned long)(b - a)));
  server.send(200, "text/plain", out);
}

// LEDC: Kanal 1 mit freien Parametern konfigurieren, an Pin 9 haengen und schreiben.
void handleLedc() {
  String out;
  uint8_t ch = (uint8_t)server.arg("ch").toInt();
  line(out, "ret", String((unsigned long)ledcSetup(ch, (uint32_t)server.arg("freq").toInt(), (uint8_t)server.arg("bits").toInt())));
  ledcAttachPin(9, ch);
  ledcWrite(ch, (uint32_t)server.arg("duty").toInt());
  server.send(200, "text/plain", out);
}

// Neu anhaengen: direkt danach darf noch keine Flanke kommen (erste frueheste nach einer Halbperiode).
void handleAttachProbe() {
  String out;
  detachInterrupt(digitalPinToInterrupt(FAN_TACH_PIN));
  delay(50);
  uint32_t start = readPulses();
  attachInterrupt(digitalPinToInterrupt(FAN_TACH_PIN), tachIsr, FALLING);
  delay(server.hasArg("ms") ? server.arg("ms").toInt() : 20);
  line(out, "early", String((unsigned long)(readPulses() - start)));
  delay(300);
  line(out, "later", String((unsigned long)(readPulses() - start)));
  server.send(200, "text/plain", out);
}

// Wiederholt anhaengen/abhaengen; nach detachInterrupt() darf kein Pulse mehr ankommen.
void handleDetachStress() {
  String out;
  int bad = 0;
  for (int i = 0; i < 150; i++) {
    attachInterrupt(digitalPinToInterrupt(FAN_TACH_PIN), tachIsr, FALLING);
    delay(1 + (i % 14));
    detachInterrupt(digitalPinToInterrupt(FAN_TACH_PIN));
    uint32_t v = readPulses();
    delayMicroseconds(300);
    delay(3);
    if (readPulses() != v) bad++;
  }
  attachInterrupt(digitalPinToInterrupt(FAN_TACH_PIN), tachIsr, FALLING);
  line(out, "bad", String(bad));
  server.send(200, "text/plain", out);
}

// Aufloesung: vor begin() 9 Bit (kurze Konversion), nach begin() mit Sensor 12 Bit.
void handleRes() {
  String out;
  DallasTemperature d(&oneWire);
  unsigned long t0 = millis();
  d.requestTemperatures();
  line(out, "before_begin_ms", String((long)(millis() - t0)));
  d.begin();
  t0 = millis();
  d.requestTemperatures();
  line(out, "after_begin_ms", String((long)(millis() - t0)));
  server.send(200, "text/plain", out);
}

// NVS-Typtags: Typabweichung liefert Standardwert bzw. 0.
void handleNvsTypes() {
  String out;
  Preferences p;
  line(out, "begin_partition", p.begin("selftest", false, "nvs") ? "1" : "0");
  p.putUInt("tu", 5);
  line(out, "int_on_uint", String((long)p.getInt("tu", -1)));
  line(out, "uint_on_uint", String((long)p.getUInt("tu", 99)));
  p.putInt("ti", -5);
  line(out, "uint_on_int", String((unsigned long)p.getUInt("ti", 99)));
  p.putString("ts", "abc");
  line(out, "bloblen_on_str", String((unsigned long)p.getBytesLength("ts")));
  uint8_t buf[16];
  line(out, "bytes_on_str", String((unsigned long)p.getBytes("ts", buf, sizeof buf)));
  line(out, "uint_on_str", String((unsigned long)p.getUInt("ts", 99)));
  uint8_t blob[3] = {65, 66, 0};
  p.putBytes("tb", blob, sizeof blob);
  line(out, "str_on_blob", p.getString("tb", "def"));
  line(out, "bytes_on_blob", String((unsigned long)p.getBytes("tb", buf, sizeof buf)));
  line(out, "bytes_nobuf", String((unsigned long)p.getBytes("tb", nullptr, 0)));
  line(out, "bytes_nobuf2", String((unsigned long)p.getBytes("tb", nullptr, 16)));
  line(out, "bytes_nolen", String((unsigned long)p.getBytes("tb", buf, 0)));
  p.putBool("tbool", true);
  line(out, "uchar_on_bool", String((int)p.getUChar("tbool", 99)));
  p.putUChar("tuc", 1);
  line(out, "bool_on_uchar", p.getBool("tuc", false) ? "1" : "0");
  p.end();
  server.send(200, "text/plain", out);
}

void handlePwm() {
  ledcWrite(0, (uint32_t)server.arg("duty").toInt());
  server.send(200, "text/plain", "ok\n");
}

void handlePin() {
  digitalWrite((uint8_t)server.arg("pin").toInt(), server.arg("v").toInt() ? HIGH : LOW);
  server.send(200, "text/plain", "ok\n");
}

void handleDetach() {
  detachInterrupt(digitalPinToInterrupt(FAN_TACH_PIN));
  server.send(200, "text/plain", "ok\n");
}

void handleAttach() {
  attachInterrupt(digitalPinToInterrupt(FAN_TACH_PIN), tachIsr, FALLING);
  server.send(200, "text/plain", "ok\n");
}

bool restartRequested = false;

void handleRestart() {
  restartRequested = true;  // Neustart erst nach der Antwort (in loop())
  server.send(200, "text/plain", "restarting\n");
}

void setup() {
  Serial.begin(115200);
  prefs.begin("selftest");
  prefs.putUInt("boots", prefs.getUInt("boots", 0) + 1);
  prefs.end();

  ledcSetup(0, 25000, 8);
  ledcAttachPin(FAN_PWM_PIN, 0);
  attachInterrupt(digitalPinToInterrupt(FAN_TACH_PIN), tachIsr, FALLING);

  WiFi.mode(WIFI_STA);
  WiFi.begin("sim", "sim");
  server.on("/", HTTP_GET, []() { server.send(200, "text/plain", "FakeSelftest\n"); });
  server.on("/t", HTTP_GET, handleTemp);
  server.on("/state", HTTP_GET, handleState);
  server.on("/nvs", HTTP_GET, handleNvs);
  server.on("/clear", HTTP_GET, handleClear);
  server.on("/pulses", HTTP_GET, handlePulses);
  server.on("/ledc", HTTP_GET, handleLedc);
  server.on("/attach_probe", HTTP_GET, handleAttachProbe);
  server.on("/detach_stress", HTTP_GET, handleDetachStress);
  server.on("/res", HTTP_GET, handleRes);
  server.on("/nvs_types", HTTP_GET, handleNvsTypes);
  server.on("/pwm", HTTP_GET, handlePwm);
  server.on("/pin", HTTP_GET, handlePin);
  server.on("/detach", HTTP_GET, handleDetach);
  server.on("/attach", HTTP_GET, handleAttach);
  server.on("/restart", HTTP_GET, handleRestart);
  server.begin();
}

void loop() {
  server.handleClient();
  if (restartRequested) ESP.restart();
  delay(2);
}
