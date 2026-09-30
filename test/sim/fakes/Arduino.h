// Host-Simulation der Arduino/ESP32-API fuer ServerWatch-Tests.
// Nur das, was die Sketches (und typische Fixes) brauchen. Verhalten orientiert
// sich an arduino-esp32 2.0.17 (PlatformIO framework-arduinoespressif32 3.20017).
#pragma once

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <string>
#include <functional>
#include <type_traits>

#define PROGMEM
#define PGM_P const char*
#define F(x) (x)
#define FPSTR(x) (x)
#define pgm_read_byte(addr) (*(const uint8_t*)(addr))
#define strlen_P strlen
#define strcpy_P strcpy
#define memcpy_P memcpy

typedef uint8_t byte;
typedef bool boolean;

#define HIGH 0x1
#define LOW 0x0
#define INPUT 0x01
#define OUTPUT 0x03
#define PULLUP 0x04
#define INPUT_PULLUP 0x05
#define PULLDOWN 0x08
#define INPUT_PULLDOWN 0x09

#define DEC 10
#define HEX 16

// ---------------------------------------------------------------- String
class String {
public:
    String() {}
    String(const char* s) : s_(s ? s : "") {}
    String(const std::string& s) : s_(s) {}
    String(const String& o) = default;
    String(String&& o) = default;
    explicit String(char c) : s_(1, c) {}
    explicit String(int v, unsigned char base = 10) : s_(fmt_int((long long)v, base)) {}
    explicit String(unsigned int v, unsigned char base = 10) : s_(fmt_int((long long)v, base)) {}
    explicit String(long v, unsigned char base = 10) : s_(fmt_int((long long)v, base)) {}
    explicit String(unsigned long v, unsigned char base = 10) : s_(fmt_int((long long)v, base)) {}
    explicit String(long long v, unsigned char base = 10) : s_(fmt_int(v, base)) {}
    explicit String(unsigned long long v, unsigned char base = 10) : s_(fmt_int((long long)v, base)) {}
    explicit String(float v, unsigned int dec = 2) : s_(fmt_dbl(v, dec)) {}
    explicit String(double v, unsigned int dec = 2) : s_(fmt_dbl(v, dec)) {}

    String& operator=(const String& o) = default;
    String& operator=(String&& o) = default;
    String& operator=(const char* s) { s_ = s ? s : ""; return *this; }

    const char* c_str() const { return s_.c_str(); }
    size_t length() const { return s_.size(); }  // auf dem ESP32 ist size_t == unsigned int
    bool isEmpty() const { return s_.empty(); }
    bool reserve(unsigned int n) { s_.reserve(n); return true; }
    const std::string& std() const { return s_; }

    bool concat(const String& o) { s_ += o.s_; return true; }
    bool concat(const char* o) { if (o) s_ += o; return true; }
    bool concat(char c) { s_ += c; return true; }
    bool concat(const char* o, unsigned int n) { s_.append(o, n); return true; }
    template <typename T, typename std::enable_if<std::is_arithmetic<T>::value && !std::is_same<T, char>::value, int>::type = 0>
    bool concat(T v) { s_ += String(v).s_; return true; }

    String& operator+=(const String& o) { concat(o); return *this; }
    String& operator+=(const char* o) { concat(o); return *this; }
    String& operator+=(char c) { concat(c); return *this; }
    template <typename T, typename std::enable_if<std::is_arithmetic<T>::value && !std::is_same<T, char>::value, int>::type = 0>
    String& operator+=(T v) { concat(v); return *this; }

    bool operator==(const String& o) const { return s_ == o.s_; }
    bool operator==(const char* o) const { return s_ == (o ? o : ""); }
    bool operator!=(const String& o) const { return s_ != o.s_; }
    bool operator!=(const char* o) const { return !(*this == o); }
    bool operator<(const String& o) const { return s_ < o.s_; }
    bool operator>(const String& o) const { return s_ > o.s_; }
    bool equals(const String& o) const { return s_ == o.s_; }
    bool equalsIgnoreCase(const String& o) const {
        if (s_.size() != o.s_.size()) return false;
        for (size_t i = 0; i < s_.size(); i++) if (tolower(s_[i]) != tolower(o.s_[i])) return false;
        return true;
    }
    char charAt(unsigned int i) const { return i < s_.size() ? s_[i] : 0; }
    char operator[](unsigned int i) const { return charAt(i); }
    char& operator[](unsigned int i) { return s_[i]; }
    void setCharAt(unsigned int i, char c) { if (i < s_.size()) s_[i] = c; }

    int indexOf(char c, unsigned int from = 0) const { auto p = s_.find(c, from); return p == std::string::npos ? -1 : (int)p; }
    int indexOf(const String& t, unsigned int from = 0) const { auto p = s_.find(t.s_, from); return p == std::string::npos ? -1 : (int)p; }
    int lastIndexOf(char c) const { auto p = s_.rfind(c); return p == std::string::npos ? -1 : (int)p; }
    int lastIndexOf(const String& t) const { auto p = s_.rfind(t.s_); return p == std::string::npos ? -1 : (int)p; }
    String substring(unsigned int from) const { return from >= s_.size() ? String() : String(s_.substr(from)); }
    String substring(unsigned int from, unsigned int to) const {
        if (from > to) std::swap(from, to);
        if (from >= s_.size()) return String();
        return String(s_.substr(from, to - from));
    }
    bool startsWith(const String& p) const { return s_.compare(0, p.s_.size(), p.s_) == 0; }
    bool endsWith(const String& p) const { return s_.size() >= p.s_.size() && s_.compare(s_.size() - p.s_.size(), p.s_.size(), p.s_) == 0; }
    void replace(const String& from, const String& to) {
        if (from.s_.empty()) return;
        size_t pos = 0;
        while ((pos = s_.find(from.s_, pos)) != std::string::npos) { s_.replace(pos, from.s_.size(), to.s_); pos += to.s_.size(); }
    }
    void replace(char from, char to) { for (auto& c : s_) if (c == from) c = to; }
    void remove(unsigned int idx) { if (idx < s_.size()) s_.erase(idx); }
    void remove(unsigned int idx, unsigned int n) { if (idx < s_.size()) s_.erase(idx, n); }
    void toLowerCase() { for (auto& c : s_) c = (char)tolower((unsigned char)c); }
    void toUpperCase() { for (auto& c : s_) c = (char)toupper((unsigned char)c); }
    void trim() {
        size_t a = s_.find_first_not_of(" \t\r\n"), b = s_.find_last_not_of(" \t\r\n");
        s_ = a == std::string::npos ? "" : s_.substr(a, b - a + 1);
    }
    long toInt() const { return atol(s_.c_str()); }
    float toFloat() const { return (float)atof(s_.c_str()); }
    double toDouble() const { return atof(s_.c_str()); }

private:
    static std::string fmt_int(long long v, unsigned char base) {
        char buf[72];
        if (base == 16) snprintf(buf, sizeof buf, "%llx", v);
        else snprintf(buf, sizeof buf, "%lld", v);
        return buf;
    }
    static std::string fmt_dbl(double v, unsigned int dec) {
        char buf[64];
        snprintf(buf, sizeof buf, "%.*f", (int)dec, v);
        return buf;
    }
    std::string s_;
};

inline String operator+(const String& a, const String& b) { String r(a); r += b; return r; }
inline String operator+(const String& a, const char* b) { String r(a); r += b; return r; }
inline String operator+(const char* a, const String& b) { String r(a); r += b; return r; }
inline String operator+(const String& a, char b) { String r(a); r += b; return r; }
template <typename T, typename std::enable_if<std::is_arithmetic<T>::value && !std::is_same<T, char>::value, int>::type = 0>
inline String operator+(const String& a, T b) { String r(a); r += b; return r; }
inline bool operator==(const char* a, const String& b) { return b == a; }

// ---------------------------------------------------------------- IPAddress
class IPAddress {
public:
    IPAddress() : a_(0) {}
    IPAddress(uint8_t a, uint8_t b, uint8_t c, uint8_t d) : a_((uint32_t)a | (uint32_t)b << 8 | (uint32_t)c << 16 | (uint32_t)d << 24) {}
    IPAddress(uint32_t a) : a_(a) {}
    bool fromString(const char* s) {
        unsigned a, b, c, d; char tail;
        if (!s || sscanf(s, "%u.%u.%u.%u%c", &a, &b, &c, &d, &tail) != 4 || a > 255 || b > 255 || c > 255 || d > 255) return false;
        *this = IPAddress(a, b, c, d);
        return true;
    }
    bool fromString(const String& s) { return fromString(s.c_str()); }
    String toString() const {
        char buf[20];
        snprintf(buf, sizeof buf, "%u.%u.%u.%u", a_ & 255, (a_ >> 8) & 255, (a_ >> 16) & 255, a_ >> 24);
        return String(buf);
    }
    operator uint32_t() const { return a_; }
    uint8_t operator[](int i) const { return (a_ >> (8 * i)) & 255; }
    bool operator==(const IPAddress& o) const { return a_ == o.a_; }
    bool operator!=(const IPAddress& o) const { return a_ != o.a_; }
private:
    uint32_t a_;
};

// ---------------------------------------------------------------- Serial
class HardwareSerial {
public:
    void begin(unsigned long) {}
    void end() {}
    void flush() {}
    int available() { return 0; }
    int read() { return -1; }
    operator bool() const { return true; }
    size_t print(const String& s) { return out(s.c_str()); }
    size_t print(const char* s) { return out(s); }
    size_t print(char c) { char b[2] = {c, 0}; return out(b); }
    size_t print(const IPAddress& ip) { return out(ip.toString().c_str()); }
    template <typename T, typename std::enable_if<std::is_arithmetic<T>::value && !std::is_same<T, char>::value, int>::type = 0>
    size_t print(T v, int base = DEC) { return out(String(v, base).c_str()); }
    size_t println() { return out("\n"); }
    template <typename T> size_t println(const T& v) { size_t n = print(v); return n + out("\n"); }
    size_t printf(const char* fmt, ...) __attribute__((format(printf, 2, 3)));
    size_t write(uint8_t c) { return print((char)c); }
private:
    size_t out(const char* s);
};
extern HardwareSerial Serial;

// ---------------------------------------------------------------- Zeit/GPIO
unsigned long millis();
unsigned long micros();
void delay(unsigned long ms);
void delayMicroseconds(unsigned int us);
void yield();
void pinMode(uint8_t pin, uint8_t mode);
void digitalWrite(uint8_t pin, uint8_t val);
int digitalRead(uint8_t pin);
int analogRead(uint8_t pin);
long random(long max);
long random(long min, long max);
void randomSeed(unsigned long);

template <typename T, typename U> auto min(T a, U b) -> decltype(a < b ? a : b) { return a < b ? a : b; }
template <typename T, typename U> auto max(T a, U b) -> decltype(a > b ? a : b) { return a > b ? a : b; }
template <typename T> T constrain(T x, T lo, T hi) { return x < lo ? lo : (x > hi ? hi : x); }

// ---------------------------------------------------------------- ESP
class EspClass {
public:
    [[noreturn]] void restart();
    uint32_t getFreeHeap();
    uint32_t getMinFreeHeap();
    uint32_t getHeapSize() { return 327680; }
    uint32_t getMaxAllocHeap() { return getFreeHeap(); }
    const char* getSdkVersion() { return "sim"; }
    const char* getChipModel() { return "ESP32-C3 (sim)"; }
    uint64_t getEfuseMac() { return 0x112233445566ULL; }
    uint32_t getChipId() { return 0x445566; }
};
extern EspClass ESP;

typedef enum { ESP_RST_UNKNOWN, ESP_RST_POWERON, ESP_RST_EXT, ESP_RST_SW, ESP_RST_PANIC, ESP_RST_INT_WDT, ESP_RST_TASK_WDT, ESP_RST_WDT, ESP_RST_DEEPSLEEP, ESP_RST_BROWNOUT, ESP_RST_SDIO } esp_reset_reason_t;
esp_reset_reason_t esp_reset_reason();

// Loop-Watchdog wie in arduino-esp32 (enableLoopWDT fuettert nach jedem loop()).
void enableLoopWDT();
void disableLoopWDT();
void feedLoopWDT();

// ---------------------------------------------------------------- FreeRTOS (minimal)
typedef void* TaskHandle_t;
typedef void* SemaphoreHandle_t;
typedef uint32_t TickType_t;
typedef int BaseType_t;
typedef unsigned int UBaseType_t;
#define pdTRUE 1
#define pdFALSE 0
#define pdPASS 1
#define portMAX_DELAY 0xffffffffu
#define portTICK_PERIOD_MS 1
#define pdMS_TO_TICKS(ms) ((TickType_t)(ms))
#define tskIDLE_PRIORITY 0
typedef void (*TaskFunction_t)(void*);
BaseType_t xTaskCreate(TaskFunction_t fn, const char* name, uint32_t stack, void* arg, UBaseType_t prio, TaskHandle_t* handle);
BaseType_t xTaskCreatePinnedToCore(TaskFunction_t fn, const char* name, uint32_t stack, void* arg, UBaseType_t prio, TaskHandle_t* handle, BaseType_t core);
void vTaskDelay(TickType_t ticks);
void vTaskDelete(TaskHandle_t h);
TickType_t xTaskGetTickCount();
SemaphoreHandle_t xSemaphoreCreateMutex();
SemaphoreHandle_t xSemaphoreCreateRecursiveMutex();
BaseType_t xSemaphoreTake(SemaphoreHandle_t s, TickType_t ticks);
BaseType_t xSemaphoreGive(SemaphoreHandle_t s);
BaseType_t xSemaphoreTakeRecursive(SemaphoreHandle_t s, TickType_t ticks);
BaseType_t xSemaphoreGiveRecursive(SemaphoreHandle_t s);

// ---------------------------------------------------------------- Sim-intern
// Vom Build-Skript eingesetzt: top-level `const char* X = "..."` -> sim_cfg("X", "...")
// Ueberschreibbar per Umgebungsvariable SWCFG_X.
const char* sim_cfg(const char* name, const char* def);

namespace sim {
// Simulierte Zeit in ms seit "Boot" (Echtzeit * SIM_TIME_SCALE).
double now_ms();
// Blockiert fuer sim_ms simulierte Millisekunden.
void sleep_sim_ms(double sim_ms);
// Wandelt simulierte ms in echte ms um.
double to_real_ms(double sim_ms);
const std::string& node_ip();
const std::string& node_dir();
const std::string& state_dir();
int map_port(int port);
std::string env(const char* key, const char* def);
std::string read_file(const std::string& path, const std::string& def = "");
void write_file(const std::string& path, const std::string& content);
void append_file(const std::string& path, const std::string& content);
bool process_alive(int pid);
// Netzwerkmodell
bool wifi_link_up();                                   // eigenes WLAN verbunden?
bool node_reachable(const std::string& ip);            // anderer Sim-Knoten erreichbar?
// TCP-Verbindung wie lwIP: liefert fd (>=0) oder -1. timeout in simulierten ms.
int tcp_connect(const std::string& host_or_ip, int port, double timeout_sim_ms);
std::string resolve(const std::string& host);          // IP oder "" (mDNS .local ueber Registry)
void log(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
void event(const std::string& line);                   // Maschinenlesbares Ereignis in events.log
}  // namespace sim
