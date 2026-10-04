// Attrappe von DallasTemperature (DS18B20), ein Sensor an Index 0.
// Messwert aus nodes/<ip>/temp: Zahl -> Wert; "disconnected", ungueltig oder Datei fehlt -> kein Sensor.
//  - getDeviceCount() wird wie in der Bibliothek bei begin() ermittelt (Bus-Scan), nicht live.
//  - Aufloesung: 9 Bit vor begin(); begin() setzt 12 Bit (Sensorstandard), wenn ein Sensor da ist.
//  - getTempCByIndex() liest wie die echte Lesung live: Sensor weg -> DEVICE_DISCONNECTED_C.
//  - Der Messwert wird erst nach Ablauf der Konversionszeit uebernommen; vorher (und vor der ersten
//    Konversion) liefert der Sensor wie echte DS18B20 den Einschaltwert 85.0.
#pragma once
#include "Arduino.h"
#include "OneWire.h"

#define DEVICE_DISCONNECTED_C -127
#define DEVICE_DISCONNECTED_F -196.6
#define DEVICE_DISCONNECTED_RAW -7040

class DallasTemperature {
public:
    DallasTemperature() {}
    DallasTemperature(OneWire*) {}

    void begin() { count_ = present() ? 1 : 0; if (count_) bits_ = 12; }  // wie die Bibliothek: Aufloesung vom Sensor lesen
    uint8_t getDeviceCount() { return count_; }
    void setResolution(uint8_t bits) { bits_ = bits < 9 ? 9 : (bits > 12 ? 12 : bits); }
    void setWaitForConversion(bool w) { wait_ = w; }

    bool requestTemperatures() {
        if (count_ == 0) { if (wait_) sim::sleep_sim_ms(convMs()); return true; }
        update();  // eine laufende Konversion zuerst abschliessen
        started_ = sim::now_ms();
        pending_ = true;
        if (wait_) sim::sleep_sim_ms(convMs());
        return true;
    }
    bool isConversionComplete() { return !pending_ || sim::now_ms() - started_ >= convMs(); }
    float getTempCByIndex(uint8_t index) {
        if (index >= count_ || !present()) return DEVICE_DISCONNECTED_C;
        update();
        return latched_;
    }

private:
    double convMs() const { return (double)(750 >> (12 - bits_)); }
    static bool read_temp(double& v) {
        std::string s = sim::read_file(sim::node_dir() + "/temp", "");
        if (s.empty()) return false;
        char* end = nullptr;
        v = strtod(s.c_str(), &end);
        return end != s.c_str() && *end == 0 && std::isfinite(v);
    }
    static bool present() { double v; return read_temp(v); }
    void update() {
        if (pending_ && isConversionComplete()) {
            double v;
            if (read_temp(v)) latched_ = (float)v;
            pending_ = false;
        }
    }
    uint8_t count_ = 0, bits_ = 9;  // 9 Bit bis begin() (Bibliotheksstandard)
    bool wait_ = true, pending_ = false;
    double started_ = 0;
    float latched_ = 85.0f;
};
