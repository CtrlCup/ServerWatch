// Laufzeit der ServerWatch-Host-Simulation.
//
// Jeder simulierte ESP ist ein eigener Prozess mit eigener Loopback-IP (127.0.10.x).
// Gemeinsamer Zustand liegt im Verzeichnis $SW_STATE:
//   nodes/<ip>/ap          "up" | "down [dropReason] [attemptReason]"   (Test -> Knoten)
//   nodes/<ip>/link        "1" | "0"   aktueller WLAN-Link              (Knoten -> Test)
//   nodes/<ip>/gpio_in_<n> "1" | "0"   Eingangspegel                    (Test -> Knoten)
//   nodes/<ip>/events.log  Ereignisse (gpio, restart, wifi, ...)        (Knoten -> Test)
//   nodes/<ip>/metrics     loops=<n> max_loop_ms=<x> ...                (Knoten -> Test)
//   nodes/<ip>/pid
//   hosts/<ip>[_<port>]    "up" | "refused" | "blackhole"  ueberwachter Server
//   mdns/<hostname>.rec    mDNS-Registry
//
// Zeit: millis() = Echtzeit * SIM_TIME_SCALE. Alle modellierten Wartezeiten (Timeouts,
// mDNS-Query, lwIP-SYN-Retries, WLAN-Verbindungsaufbau) laufen in simulierter Zeit.
#include "Arduino.h"
#include "WiFi.h"
#include "WebServer.h"
#include "ESPmDNS.h"
#include "HTTPClient.h"
#include "WebSocketsServer.h"
#include "esp_task_wdt.h"

#include <algorithm>
#include <arpa/inet.h>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdarg>
#include <dirent.h>
#include <fcntl.h>
#include <fstream>
#include <malloc.h>
#include <map>
#include <mutex>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <signal.h>
#include <sstream>
#include <sys/socket.h>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>

HardwareSerial Serial;
EspClass ESP;
WiFiClass WiFi;
MDNSResponder MDNS;

// =====================================================================================
// Grundlagen: Konfiguration, Zeit, Dateien, Logging
// =====================================================================================
namespace sim {
static std::chrono::steady_clock::time_point g_t0 = std::chrono::steady_clock::now();
static double g_scale = 1.0;
static std::string g_ip = "127.0.10.1", g_state = "/tmp/sw-sim", g_node_dir;
static int g_port_offset = 10000;
static std::mutex g_log_mutex;
char** g_argv = nullptr;

std::string env(const char* key, const char* def) {
    const char* v = getenv(key);
    return v && *v ? std::string(v) : std::string(def);
}
static double env_d(const char* key, double def) {
    const char* v = getenv(key);
    return v && *v ? atof(v) : def;
}

double now_ms() {
    auto d = std::chrono::steady_clock::now() - g_t0;
    return std::chrono::duration<double, std::milli>(d).count() * g_scale;
}
double to_real_ms(double sim_ms) { return sim_ms / g_scale; }
void sleep_sim_ms(double sim_ms) {
    if (sim_ms <= 0) return;
    std::this_thread::sleep_for(std::chrono::microseconds((long long)(to_real_ms(sim_ms) * 1000.0)));
}
const std::string& node_ip() { return g_ip; }
const std::string& node_dir() { return g_node_dir; }
const std::string& state_dir() { return g_state; }
int map_port(int port) { return port + g_port_offset; }

static void mkdirs(const std::string& path) {
    std::string cur;
    std::stringstream ss(path);
    std::string part;
    if (!path.empty() && path[0] == '/') cur = "/";
    while (std::getline(ss, part, '/')) {
        if (part.empty()) continue;
        cur += part + "/";
        mkdir(cur.c_str(), 0755);
    }
}
std::string read_file(const std::string& path, const std::string& def) {
    std::ifstream f(path);
    if (!f) return def;
    std::stringstream ss;
    ss << f.rdbuf();
    std::string s = ss.str();
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' ')) s.pop_back();
    return s;
}
void write_file(const std::string& path, const std::string& content) {
    std::string tmp = path + ".tmp" + std::to_string(getpid()) + "_" + std::to_string(std::hash<std::thread::id>()(std::this_thread::get_id()) % 100000);
    { std::ofstream f(tmp); f << content; }
    rename(tmp.c_str(), path.c_str());
}
void append_file(const std::string& path, const std::string& content) {
    std::lock_guard<std::mutex> lk(g_log_mutex);
    std::ofstream f(path, std::ios::app);
    f << content;
}
bool process_alive(int pid) { return pid > 0 && (kill(pid, 0) == 0 || errno == EPERM); }

void log(const char* fmt, ...) {
    char buf[2048];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    std::lock_guard<std::mutex> lk(g_log_mutex);
    fprintf(stdout, "[%10.0f][sim] %s\n", now_ms(), buf);
    fflush(stdout);
}
void event(const std::string& line) {
    char ts[32];
    snprintf(ts, sizeof ts, "%.0f ", now_ms());
    append_file(g_node_dir + "/events.log", ts + line + "\n");
}

void init(int argc, char** argv) {
    (void)argc;
    g_argv = argv;
    g_scale = env_d("SIM_TIME_SCALE", 1.0);
    if (g_scale <= 0) g_scale = 1.0;
    g_ip = env("SW_IP", "127.0.10.1");
    g_state = env("SW_STATE", "/tmp/sw-sim");
    g_port_offset = atoi(env("SW_PORT_OFFSET", "10000").c_str());
    g_node_dir = g_state + "/nodes/" + g_ip;
    mkdirs(g_node_dir);
    mkdirs(g_state + "/mdns");
    mkdirs(g_state + "/hosts");
    write_file(g_node_dir + "/pid", std::to_string(getpid()));
    write_file(g_node_dir + "/link", "0");
    signal(SIGPIPE, SIG_IGN);
}
}  // namespace sim

const char* sim_cfg(const char* name, const char* def) {
    std::string key = std::string("SWCFG_") + name;
    const char* v = getenv(key.c_str());
    return v ? strdup(v) : def;
}

bool sim_cfg_bool(const char* name, bool def) {
    std::string key = std::string("SWCFG_") + name;
    const char* v = getenv(key.c_str());
    if (!v) return def;
    return strcmp(v, "true") == 0 || strcmp(v, "1") == 0;
}

size_t HardwareSerial::printf(const char* fmt, ...) {
    char buf[4096];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    out(buf);
    return n > 0 ? (size_t)n : 0;
}
size_t HardwareSerial::out(const char* s) {
    static bool bol = true;
    std::lock_guard<std::mutex> lk(sim::g_log_mutex);
    size_t n = 0;
    for (const char* p = s; *p; ++p, ++n) {
        if (bol) { fprintf(stdout, "[%10.0f] ", sim::now_ms()); bol = false; }
        fputc(*p, stdout);
        if (*p == '\n') bol = true;
    }
    fflush(stdout);
    return n;
}

unsigned long millis() { return (unsigned long)sim::now_ms(); }
unsigned long micros() { return (unsigned long)(sim::now_ms() * 1000.0); }
void delay(unsigned long ms) { sim::sleep_sim_ms((double)ms); }
void delayMicroseconds(unsigned int us) { sim::sleep_sim_ms(us / 1000.0); }
void yield() { std::this_thread::yield(); }
long random(long max) { return max > 0 ? rand() % max : 0; }
long random(long min, long max) { return max > min ? min + rand() % (max - min) : min; }
void randomSeed(unsigned long s) { srand((unsigned)s); }

// ---------------------------------------------------------------- GPIO
static std::mutex g_gpio_mutex;
static std::map<int, int> g_pin_mode, g_pin_out;

void pinMode(uint8_t pin, uint8_t mode) {
    std::lock_guard<std::mutex> lk(g_gpio_mutex);
    g_pin_mode[pin] = mode;
    sim::event("pinmode " + std::to_string(pin) + " " + std::to_string(mode));
}
void digitalWrite(uint8_t pin, uint8_t val) {
    {
        std::lock_guard<std::mutex> lk(g_gpio_mutex);
        g_pin_out[pin] = val ? 1 : 0;
    }
    sim::event("gpio " + std::to_string(pin) + " " + std::to_string(val ? 1 : 0));
}
int digitalRead(uint8_t pin) {
    int mode;
    {
        std::lock_guard<std::mutex> lk(g_gpio_mutex);
        mode = g_pin_mode.count(pin) ? g_pin_mode[pin] : INPUT;
        if (mode == OUTPUT) return g_pin_out[pin];
    }
    std::string v = sim::read_file(sim::node_dir() + "/gpio_in_" + std::to_string(pin), "");
    if (v.empty()) return mode == INPUT_PULLUP ? HIGH : LOW;
    return v == "1" ? HIGH : LOW;
}
int analogRead(uint8_t pin) { return digitalRead(pin) ? 4095 : 0; }

// ---------------------------------------------------------------- ESP / Reset / Heap
static esp_reset_reason_t g_reset_reason = ESP_RST_POWERON;

namespace sim {
[[noreturn]] void reboot(esp_reset_reason_t reason, const char* why) {
    event(std::string("restart ") + why);
    log("*** Neustart (%s) ***", why);
    fflush(stdout);
    setenv("SIM_RESET_REASON", std::to_string((int)reason).c_str(), 1);
    std::string boots = read_file(g_node_dir + "/boots", "0");
    write_file(g_node_dir + "/boots", std::to_string(atoi(boots.c_str()) + 1));
    // Alle Sockets schliessen, damit der neue Prozess die Ports binden kann.
    for (int fd = 3; fd < 1024; fd++) close(fd);
    execv("/proc/self/exe", g_argv);
    _exit(3);
}
}  // namespace sim

void EspClass::restart() { sim::reboot(ESP_RST_SW, "ESP.restart"); }
static size_t g_heap_baseline = 0;
uint32_t EspClass::getFreeHeap() {
    struct mallinfo2 mi = mallinfo2();
    long used = (long)mi.uordblks - (long)g_heap_baseline;
    long free_ = 250000 - used;
    return free_ > 0 ? (uint32_t)free_ : 0;
}
uint32_t EspClass::getMinFreeHeap() { return getFreeHeap(); }
esp_reset_reason_t esp_reset_reason() { return g_reset_reason; }

// ---------------------------------------------------------------- Watchdog
static std::mutex g_wdt_mutex;
static std::map<std::thread::id, double> g_wdt_tasks;  // Task -> letzte Fuetterung (sim ms)
static double g_wdt_timeout_ms = 5000;                  // CONFIG_ESP_TASK_WDT_TIMEOUT_S = 5
static std::thread::id g_loop_thread;

esp_err_t esp_task_wdt_init(uint32_t timeout_s, bool) {
    std::lock_guard<std::mutex> lk(g_wdt_mutex);
    g_wdt_timeout_ms = timeout_s * 1000.0;
    return ESP_OK;
}
esp_err_t esp_task_wdt_init(const esp_task_wdt_config_t* cfg) {
    std::lock_guard<std::mutex> lk(g_wdt_mutex);
    g_wdt_timeout_ms = cfg->timeout_ms;
    return ESP_OK;
}
esp_err_t esp_task_wdt_reconfigure(const esp_task_wdt_config_t* cfg) { return esp_task_wdt_init(cfg); }
esp_err_t esp_task_wdt_add(TaskHandle_t) {
    std::lock_guard<std::mutex> lk(g_wdt_mutex);
    g_wdt_tasks[std::this_thread::get_id()] = sim::now_ms();
    return ESP_OK;
}
esp_err_t esp_task_wdt_delete(TaskHandle_t) {
    std::lock_guard<std::mutex> lk(g_wdt_mutex);
    g_wdt_tasks.erase(std::this_thread::get_id());
    return ESP_OK;
}
esp_err_t esp_task_wdt_reset() {
    std::lock_guard<std::mutex> lk(g_wdt_mutex);
    auto it = g_wdt_tasks.find(std::this_thread::get_id());
    if (it == g_wdt_tasks.end()) return ESP_FAIL;
    it->second = sim::now_ms();
    return ESP_OK;
}
esp_err_t esp_task_wdt_deinit() {
    std::lock_guard<std::mutex> lk(g_wdt_mutex);
    g_wdt_tasks.clear();
    return ESP_OK;
}
static bool g_loop_wdt = false;
void enableLoopWDT() {
    std::lock_guard<std::mutex> lk(g_wdt_mutex);
    g_loop_wdt = true;
    g_wdt_tasks[g_loop_thread] = sim::now_ms();
}
void disableLoopWDT() {
    std::lock_guard<std::mutex> lk(g_wdt_mutex);
    g_loop_wdt = false;
    g_wdt_tasks.erase(g_loop_thread);
}
void feedLoopWDT() {
    std::lock_guard<std::mutex> lk(g_wdt_mutex);
    if (g_loop_wdt) g_wdt_tasks[g_loop_thread] = sim::now_ms();
}
static void wdt_thread() {
    for (;;) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        bool fire = false;
        {
            std::lock_guard<std::mutex> lk(g_wdt_mutex);
            double now = sim::now_ms();
            for (auto& t : g_wdt_tasks)
                if (now - t.second > g_wdt_timeout_ms) fire = true;
        }
        if (fire) sim::reboot(ESP_RST_TASK_WDT, "task_wdt");
    }
}

// ---------------------------------------------------------------- FreeRTOS
BaseType_t xTaskCreate(TaskFunction_t fn, const char*, uint32_t, void* arg, UBaseType_t, TaskHandle_t* handle) {
    std::thread t(fn, arg);
    if (handle) *handle = (TaskHandle_t)(uintptr_t)std::hash<std::thread::id>()(t.get_id());
    t.detach();
    return pdPASS;
}
BaseType_t xTaskCreatePinnedToCore(TaskFunction_t fn, const char* name, uint32_t stack, void* arg, UBaseType_t prio, TaskHandle_t* handle, BaseType_t) {
    return xTaskCreate(fn, name, stack, arg, prio, handle);
}
void vTaskDelay(TickType_t ticks) { sim::sleep_sim_ms((double)ticks); }
void vTaskDelete(TaskHandle_t h) {
    if (h == nullptr) {
        // aktuelle Task beenden: Thread schlafen legen (detached Threads koennen nicht sauber enden)
        for (;;) std::this_thread::sleep_for(std::chrono::hours(1));
    }
}
TickType_t xTaskGetTickCount() { return (TickType_t)sim::now_ms(); }
SemaphoreHandle_t xSemaphoreCreateMutex() { return new std::recursive_timed_mutex(); }
SemaphoreHandle_t xSemaphoreCreateRecursiveMutex() { return new std::recursive_timed_mutex(); }
BaseType_t xSemaphoreTake(SemaphoreHandle_t s, TickType_t ticks) {
    auto* m = (std::recursive_timed_mutex*)s;
    if (ticks == portMAX_DELAY) { m->lock(); return pdTRUE; }
    return m->try_lock_for(std::chrono::microseconds((long long)(sim::to_real_ms(ticks) * 1000))) ? pdTRUE : pdFALSE;
}
BaseType_t xSemaphoreGive(SemaphoreHandle_t s) { ((std::recursive_timed_mutex*)s)->unlock(); return pdTRUE; }
BaseType_t xSemaphoreTakeRecursive(SemaphoreHandle_t s, TickType_t t) { return xSemaphoreTake(s, t); }
BaseType_t xSemaphoreGiveRecursive(SemaphoreHandle_t s) { return xSemaphoreGive(s); }

// =====================================================================================
// WLAN-Modell (orientiert an WiFiGeneric.cpp aus arduino-esp32 2.0.17)
// =====================================================================================
namespace {
std::recursive_mutex g_wifi_mutex;
struct WifiState {
    bool connected = false;
    wl_status_t status = WL_NO_SHIELD;
    bool autoReconnect = true;
    bool firstConnect = true;
    double attemptStart = -1;  // laufender Verbindungsversuch seit (sim ms), -1 = keiner
    bool linkWritten = false;
    int disconnects = 0;
} g_wifi;
struct EvCb { wifi_event_id_t id; WiFiEventFuncCb fn; WiFiEventCb plain; arduino_event_id_t filter; };
std::vector<EvCb> g_ev_cbs;
wifi_event_id_t g_ev_next = 1;
std::atomic<bool> g_link{false};

double connect_ms() { return atof(sim::env("SIM_WIFI_CONNECT_MS", "3000").c_str()); }
double scan_fail_ms() { return atof(sim::env("SIM_WIFI_SCAN_FAIL_MS", "2500").c_str()); }

struct ApState { bool up; int dropReason; int attemptReason; };
ApState read_ap() {
    std::string s = sim::read_file(sim::node_dir() + "/ap", "up");
    ApState a{true, WIFI_REASON_BEACON_TIMEOUT, WIFI_REASON_NO_AP_FOUND};
    std::stringstream ss(s);
    std::string word;
    ss >> word;
    a.up = word != "down";
    int r1 = 0, r2 = 0;
    if (ss >> r1) a.dropReason = r1;
    if (ss >> r2) a.attemptReason = r2;
    return a;
}
bool reconnectable(int r) {
    switch (r) {
        case 1: case 2: case 15: case 16: case 23: case 204: case 3: case 4: case 5: case 6: case 7: case 9:
        case 14: case 17: case 53: case 200: case 201: case 203: case 205: case 206: case 207:
            return true;
        default:
            return false;
    }
}
void set_link(bool up) {
    if (g_link.load() == up && g_wifi.linkWritten) return;
    g_link = up;
    g_wifi.linkWritten = true;
    sim::write_file(sim::node_dir() + "/link", up ? "1" : "0");
    sim::event(std::string("link ") + (up ? "up" : "down"));
}
void fire(arduino_event_id_t ev, arduino_event_info_t info) {
    std::vector<EvCb> cbs = g_ev_cbs;
    for (auto& c : cbs) {
        if (c.filter != ARDUINO_EVENT_MAX && c.filter != ev) continue;
        if (c.fn) c.fn(ev, info);
        else if (c.plain) c.plain(ev);
    }
}
void start_attempt() { g_wifi.attemptStart = sim::now_ms(); }

// Nachbildung von WiFiGenericClass::_eventCallback fuer STA_DISCONNECTED
void on_disconnected(int reason) {
    g_wifi.connected = false;
    g_wifi.disconnects++;
    set_link(false);
    if (reason == WIFI_REASON_NO_AP_FOUND) g_wifi.status = WL_NO_SSID_AVAIL;
    else if (reason == WIFI_REASON_AUTH_FAIL && !g_wifi.firstConnect) g_wifi.status = WL_CONNECT_FAILED;
    else if (reason == WIFI_REASON_BEACON_TIMEOUT || reason == WIFI_REASON_HANDSHAKE_TIMEOUT) g_wifi.status = WL_CONNECTION_LOST;
    else if (reason == WIFI_REASON_AUTH_EXPIRE) { /* Status bleibt unveraendert (!) */ }
    else g_wifi.status = WL_DISCONNECTED;
    sim::event("wifi_disconnected " + std::to_string(reason));
    arduino_event_info_t info{};
    info.wifi_sta_disconnected.reason = (uint8_t)reason;
    fire(ARDUINO_EVENT_WIFI_STA_DISCONNECTED, info);

    bool doReconnect = false;
    if (reason == WIFI_REASON_ASSOC_LEAVE) {
    } else if (g_wifi.firstConnect) {
        g_wifi.firstConnect = false;
        doReconnect = true;
    } else if (g_wifi.autoReconnect && reconnectable(reason)) {
        doReconnect = true;
    } else if (reason == WIFI_REASON_ASSOC_FAIL) {
        g_wifi.status = WL_CONNECT_FAILED;
    }
    if (doReconnect) start_attempt();  // WiFi.disconnect(); WiFi.begin();
    else g_wifi.attemptStart = -1;
}
void on_connected() {
    g_wifi.connected = true;
    g_wifi.attemptStart = -1;
    g_wifi.status = WL_CONNECTED;
    set_link(true);
    sim::event("wifi_connected");
    arduino_event_info_t info{};
    fire(ARDUINO_EVENT_WIFI_STA_CONNECTED, info);
    IPAddress ip;
    ip.fromString(sim::node_ip().c_str());
    info.got_ip.ip_info.ip.addr = (uint32_t)ip;
    fire(ARDUINO_EVENT_WIFI_STA_GOT_IP, info);
}
void wifi_thread() {
    for (;;) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        WiFi.sim_tick();
    }
}
}  // namespace

void WiFiClass::sim_tick() {
    std::lock_guard<std::recursive_mutex> lk(g_wifi_mutex);
    ApState ap = read_ap();
    double now = sim::now_ms();
    if (g_wifi.connected && !ap.up) {
        on_disconnected(ap.dropReason);
        return;
    }
    if (!g_wifi.connected && g_wifi.attemptStart >= 0) {
        if (ap.up && now - g_wifi.attemptStart >= connect_ms()) on_connected();
        else if (!ap.up && now - g_wifi.attemptStart >= scan_fail_ms()) on_disconnected(ap.attemptReason);
    }
}
bool WiFiClass::setAutoReconnect(bool a) { std::lock_guard<std::recursive_mutex> lk(g_wifi_mutex); g_wifi.autoReconnect = a; return true; }
bool WiFiClass::getAutoReconnect() { std::lock_guard<std::recursive_mutex> lk(g_wifi_mutex); return g_wifi.autoReconnect; }
wl_status_t WiFiClass::begin(const char* ssid, const char*, int32_t, const uint8_t*, bool connect) {
    std::lock_guard<std::recursive_mutex> lk(g_wifi_mutex);
    mode_ = WIFI_STA;
    ssid_ = ssid ? ssid : "";
    if (g_wifi.status == WL_NO_SHIELD || g_wifi.status == WL_STOPPED) g_wifi.status = WL_DISCONNECTED;
    if (g_wifi.connected) return WL_CONNECTED;
    sim::event("wifi_begin");
    if (connect) start_attempt();
    return g_wifi.status;
}
wl_status_t WiFiClass::begin() { return begin(ssid_.c_str()); }
bool WiFiClass::disconnect(bool, bool) {
    std::lock_guard<std::recursive_mutex> lk(g_wifi_mutex);
    sim::event("wifi_disconnect_called");
    if (g_wifi.connected || g_wifi.attemptStart >= 0) {
        g_wifi.attemptStart = -1;
        on_disconnected(WIFI_REASON_ASSOC_LEAVE);
    }
    return true;
}
bool WiFiClass::reconnect() { disconnect(); begin(); return true; }
wl_status_t WiFiClass::status() { std::lock_guard<std::recursive_mutex> lk(g_wifi_mutex); return g_wifi.status; }
uint8_t WiFiClass::waitForConnectResult(unsigned long timeoutLength) {
    double start = sim::now_ms();
    while (sim::now_ms() - start < timeoutLength) {
        wl_status_t s = status();
        if (s != WL_DISCONNECTED && s != WL_IDLE_STATUS) return s;
        delay(100);
    }
    return status();
}
IPAddress WiFiClass::localIP() {
    IPAddress ip;
    if (g_link) ip.fromString(sim::node_ip().c_str());
    return ip;
}
String WiFiClass::macAddress() {
    IPAddress ip;
    ip.fromString(sim::node_ip().c_str());
    char buf[18];
    snprintf(buf, sizeof buf, "24:0A:C4:%02X:%02X:%02X", ip[1], ip[2], ip[3]);
    return String(buf);
}
int8_t WiFiClass::RSSI() {
    if (!g_link) return 0;
    return (int8_t)atoi(sim::read_file(sim::node_dir() + "/rssi", "-60").c_str());
}
const char* WiFiClass::disconnectReasonName(int reason) {
    switch (reason) {
        case 2: return "AUTH_EXPIRE"; case 3: return "AUTH_LEAVE"; case 4: return "ASSOC_EXPIRE"; case 8: return "ASSOC_LEAVE";
        case 15: return "4WAY_HANDSHAKE_TIMEOUT"; case 200: return "BEACON_TIMEOUT"; case 201: return "NO_AP_FOUND";
        case 202: return "AUTH_FAIL"; case 203: return "ASSOC_FAIL"; case 204: return "HANDSHAKE_TIMEOUT"; default: return "UNSPECIFIED";
    }
}
wifi_event_id_t WiFiClass::onEvent(WiFiEventCb cb, arduino_event_id_t event) {
    std::lock_guard<std::recursive_mutex> lk(g_wifi_mutex);
    g_ev_cbs.push_back({g_ev_next, nullptr, cb, event});
    return g_ev_next++;
}
wifi_event_id_t WiFiClass::onEvent(WiFiEventFuncCb cb, arduino_event_id_t event) {
    std::lock_guard<std::recursive_mutex> lk(g_wifi_mutex);
    g_ev_cbs.push_back({g_ev_next, cb, nullptr, event});
    return g_ev_next++;
}
void WiFiClass::removeEvent(wifi_event_id_t id) {
    std::lock_guard<std::recursive_mutex> lk(g_wifi_mutex);
    for (size_t i = 0; i < g_ev_cbs.size(); i++)
        if (g_ev_cbs[i].id == id) { g_ev_cbs.erase(g_ev_cbs.begin() + i); break; }
}
int WiFiClass::hostByName(const char* host, IPAddress& result) {
    std::string ip = sim::resolve(host ? host : "");
    if (ip.empty()) return 0;
    return result.fromString(ip.c_str()) ? 1 : 0;
}

// =====================================================================================
// Netzwerkmodell
// =====================================================================================
namespace sim {
bool wifi_link_up() { return g_link.load(); }

bool node_reachable(const std::string& ip) {
    std::string dir = g_state + "/nodes/" + ip;
    if (!process_alive(atoi(read_file(dir + "/pid", "0").c_str()))) return false;
    return read_file(dir + "/link", "0") == "1";
}

static bool is_ip(const std::string& s) { IPAddress ip; return ip.fromString(s.c_str()); }

std::string resolve(const std::string& host) {
    if (is_ip(host)) return host;
    std::string h = host;
    if (h.size() > 6 && h.substr(h.size() - 6) == ".local") h = h.substr(0, h.size() - 6);
    std::string rec = read_file(g_state + "/mdns/" + h + ".rec", "");
    if (rec.empty()) return "";
    std::stringstream ss(rec);
    std::string line, ip;
    int pid = 0;
    while (std::getline(ss, line)) {
        if (line.rfind("ip=", 0) == 0) ip = line.substr(3);
        if (line.rfind("pid=", 0) == 0) pid = atoi(line.c_str() + 4);
    }
    return process_alive(pid) && node_reachable(ip) ? ip : "";
}

// lwIP (ESP-IDF 4.4, CONFIG_LWIP_TCP_SYNMAXRTX=6, RTO 3000 ms, exponentielles Backoff) gibt
// einen unbeantworteten SYN erst nach Minuten auf. Wir modellieren konservativ mit 45 s
// (Zeitpunkt der 5. Wiederholung: 3+6+12+24 s); real dauert es eher laenger.
static double syn_giveup_ms() { return atof(env("SIM_LWIP_SYN_GIVEUP_MS", "45000").c_str()); }

static int blackhole(double timeout_sim_ms) {
    double t = timeout_sim_ms < syn_giveup_ms() ? timeout_sim_ms : syn_giveup_ms();
    sleep_sim_ms(t);
    return -1;
}

int tcp_connect(const std::string& host_or_ip, int port, double timeout_sim_ms) {
    if (!wifi_link_up()) return -1;  // keine Route
    std::string ip = resolve(host_or_ip);
    if (ip.empty()) return -1;
    if (ip.rfind("127.", 0) != 0) {
        // Virtueller Host (ueberwachter Server)
        std::string mode = read_file(g_state + "/hosts/" + ip + "_" + std::to_string(port), "");
        if (mode.empty()) mode = read_file(g_state + "/hosts/" + ip, "blackhole");
        if (mode == "up") {
            sleep_sim_ms(atof(env("SIM_SERVER_RTT_MS", "3").c_str()));
            int sv[2];
            if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0) return -1;
            close(sv[1]);
            return sv[0];
        }
        if (mode == "refused") { sleep_sim_ms(1); return -1; }
        return blackhole(timeout_sim_ms);
    }
    // Anderer Sim-Knoten: nur erreichbar, wenn dessen WLAN-Link steht
    if (!node_reachable(ip)) return blackhole(timeout_sim_ms);
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons((uint16_t)map_port(port));
    inet_pton(AF_INET, ip.c_str(), &a.sin_addr);
    int r = ::connect(fd, (sockaddr*)&a, sizeof a);
    if (r < 0 && errno != EINPROGRESS) { close(fd); return -1; }
    if (r < 0) {
        pollfd p{fd, POLLOUT, 0};
        int pr = poll(&p, 1, (int)(to_real_ms(timeout_sim_ms)) + 1);
        int err = 0;
        socklen_t len = sizeof err;
        if (pr <= 0 || getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len) != 0 || err != 0) { close(fd); return -1; }
    }
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) & ~O_NONBLOCK);
    return fd;
}

// Liest verfuegbare Daten mit Timeout (sim ms). Rueckgabe: >0 Bytes, 0 = geschlossen, -1 = Timeout/Fehler
static int recv_timeout(int fd, char* buf, size_t n, double timeout_sim_ms) {
    pollfd p{fd, POLLIN, 0};
    int pr = poll(&p, 1, (int)to_real_ms(timeout_sim_ms) + 1);
    if (pr <= 0) return -1;
    ssize_t r = recv(fd, buf, n, 0);
    return r < 0 ? -1 : (int)r;
}
static bool send_all(int fd, const std::string& data, double timeout_sim_ms) {
    size_t off = 0;
    double start = now_ms();
    while (off < data.size()) {
        ssize_t w = send(fd, data.data() + off, data.size() - off, MSG_DONTWAIT | MSG_NOSIGNAL);
        if (w > 0) { off += (size_t)w; continue; }
        if (w < 0 && errno != EAGAIN && errno != EWOULDBLOCK) return false;
        if (now_ms() - start > timeout_sim_ms) return false;
        pollfd p{fd, POLLOUT, 0};
        poll(&p, 1, 5);
    }
    return true;
}
static int listen_on(const std::string& ip, int port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons((uint16_t)map_port(port));
    inet_pton(AF_INET, ip.c_str(), &a.sin_addr);
    if (bind(fd, (sockaddr*)&a, sizeof a) != 0) {
        log("bind %s:%d fehlgeschlagen: %s", ip.c_str(), map_port(port), strerror(errno));
        _exit(10);
    }
    listen(fd, 8);  // lwIP: kleine Backlog-Queue
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
    return fd;
}
static IPAddress peer_ip(int fd) {
    sockaddr_in a{};
    socklen_t l = sizeof a;
    IPAddress ip;
    if (getpeername(fd, (sockaddr*)&a, &l) == 0) {
        char buf[32];
        inet_ntop(AF_INET, &a.sin_addr, buf, sizeof buf);
        ip.fromString(buf);
    }
    return ip;
}
static std::string url_decode(const std::string& s) {
    std::string o;
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] == '+') o += ' ';
        else if (s[i] == '%' && i + 2 < s.size()) { o += (char)strtol(s.substr(i + 1, 2).c_str(), nullptr, 16); i += 2; }
        else o += s[i];
    }
    return o;
}
static std::string lower(std::string s) { for (auto& c : s) c = (char)tolower((unsigned char)c); return s; }

static const char* B64 = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
static std::string b64enc(const unsigned char* d, size_t n) {
    std::string o;
    for (size_t i = 0; i < n; i += 3) {
        uint32_t v = d[i] << 16 | (i + 1 < n ? d[i + 1] << 8 : 0) | (i + 2 < n ? d[i + 2] : 0);
        o += B64[v >> 18 & 63];
        o += B64[v >> 12 & 63];
        o += i + 1 < n ? B64[v >> 6 & 63] : '=';
        o += i + 2 < n ? B64[v & 63] : '=';
    }
    return o;
}
static std::string b64dec(const std::string& s) {
    std::string o;
    int val = 0, bits = -8;
    for (char c : s) {
        const char* p = strchr(B64, c);
        if (!p || !c) break;
        val = (val << 6) + (int)(p - B64);
        bits += 6;
        if (bits >= 0) { o += (char)((val >> bits) & 0xFF); bits -= 8; }
    }
    return o;
}
static void sha1(const std::string& msg, unsigned char out[20]) {
    uint32_t h[5] = {0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0};
    std::string m = msg;
    uint64_t bitlen = (uint64_t)msg.size() * 8;
    m += (char)0x80;
    while (m.size() % 64 != 56) m += (char)0;
    for (int i = 7; i >= 0; i--) m += (char)(bitlen >> (i * 8));
    auto rol = [](uint32_t v, int b) { return (v << b) | (v >> (32 - b)); };
    for (size_t off = 0; off < m.size(); off += 64) {
        uint32_t w[80];
        for (int i = 0; i < 16; i++)
            w[i] = (uint32_t)(unsigned char)m[off + 4 * i] << 24 | (uint32_t)(unsigned char)m[off + 4 * i + 1] << 16 |
                   (uint32_t)(unsigned char)m[off + 4 * i + 2] << 8 | (uint32_t)(unsigned char)m[off + 4 * i + 3];
        for (int i = 16; i < 80; i++) w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
        uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
        for (int i = 0; i < 80; i++) {
            uint32_t f, k;
            if (i < 20) { f = (b & c) | (~b & d); k = 0x5A827999; }
            else if (i < 40) { f = b ^ c ^ d; k = 0x6ED9EBA1; }
            else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDC; }
            else { f = b ^ c ^ d; k = 0xCA62C1D6; }
            uint32_t t = rol(a, 5) + f + e + k + w[i];
            e = d; d = c; c = rol(b, 30); b = a; a = t;
        }
        h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e;
    }
    for (int i = 0; i < 5; i++) for (int j = 0; j < 4; j++) out[i * 4 + j] = (unsigned char)(h[i] >> (24 - 8 * j));
}
}  // namespace sim

// ---------------------------------------------------------------- WiFiClient
int WiFiClient::connect(IPAddress ip, uint16_t port, int32_t timeout_ms) { return connect(ip.toString().c_str(), port, timeout_ms); }
int WiFiClient::connect(const char* host, uint16_t port, int32_t timeout_ms) {
    stop();
    timeout_ = timeout_ms;
    fd_ = sim::tcp_connect(host ? host : "", port, (double)timeout_ms);
    return fd_ >= 0 ? 1 : 0;
}
void WiFiClient::stop() { if (fd_ >= 0) { close(fd_); fd_ = -1; } }
size_t WiFiClient::write(const uint8_t* buf, size_t n) {
    if (fd_ < 0) return 0;
    return sim::send_all(fd_, std::string((const char*)buf, n), timeout_) ? n : 0;
}
int WiFiClient::available() {
    if (fd_ < 0) return 0;
    int n = 0;
    pollfd p{fd_, POLLIN, 0};
    if (poll(&p, 1, 0) > 0) { char c; n = recv(fd_, &c, 1, MSG_PEEK | MSG_DONTWAIT) > 0 ? 1 : 0; }
    return n;
}
int WiFiClient::read() {
    char c;
    return fd_ >= 0 && recv(fd_, &c, 1, MSG_DONTWAIT) == 1 ? (unsigned char)c : -1;
}

// =====================================================================================
// WebServer
// =====================================================================================
void WebServer::begin() {
    if (listenFd_ < 0) listenFd_ = sim::listen_on(sim::node_ip(), port_);
    sim::event("webserver_begin " + std::to_string(port_));
}
void WebServer::close() { if (listenFd_ >= 0) { ::close(listenFd_); listenFd_ = -1; } }

void WebServer::handleClient() {
    // Testhilfe: simuliert einen unbekannten Haenger (z. B. Treiber-Deadlock) im ersten Boot.
    static double hangAt = atof(sim::env("SIM_HANG_AT_MS", "0").c_str());
    if (hangAt > 0 && g_reset_reason == ESP_RST_POWERON && sim::now_ms() >= hangAt) {
        sim::event("sim_hang");
        for (;;) std::this_thread::sleep_for(std::chrono::hours(1));
    }
    if (listenFd_ < 0 || !sim::wifi_link_up()) return;
    int fd = accept(listenFd_, nullptr, nullptr);
    if (fd < 0) return;
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) & ~O_NONBLOCK);
    client_.ip_ = sim::peer_ip(fd);
    responded_ = false;
    respHeaders_.clear();
    body_.clear();
    contentLength_ = CONTENT_LENGTH_NOT_SET;
    if (!readRequest(fd)) { ::close(fd); return; }
    bool handled = false;
    for (auto& r : routes_) {
        if (r.uri.std() == uri_ && (r.method == HTTP_ANY || r.method == method_)) {
            r.fn();
            handled = true;
            break;
        }
    }
    if (!handled) {
        if (notFound_) notFound_();
        else send(404, "text/plain", String("Not found: ") + uri_.c_str());
    }
    flushResponse(fd);
    ::close(fd);
}

bool WebServer::readRequest(int fd) {
    std::string buf;
    char tmp[2048];
    double start = sim::now_ms();
    size_t hdrEnd;
    while ((hdrEnd = buf.find("\r\n\r\n")) == std::string::npos) {
        double left = HTTP_MAX_DATA_WAIT - (sim::now_ms() - start);
        if (left <= 0) return false;
        int n = sim::recv_timeout(fd, tmp, sizeof tmp, left);
        if (n <= 0) return false;
        buf.append(tmp, n);
    }
    std::string head = buf.substr(0, hdrEnd), body = buf.substr(hdrEnd + 4);
    std::stringstream ss(head);
    std::string line, method, target, proto;
    std::getline(ss, line);
    std::stringstream rl(line);
    rl >> method >> target >> proto;
    static const std::map<std::string, HTTPMethod> methods = {
        {"GET", HTTP_GET}, {"POST", HTTP_POST}, {"PUT", HTTP_PUT}, {"PATCH", HTTP_PATCH},
        {"DELETE", HTTP_DELETE}, {"OPTIONS", HTTP_OPTIONS}, {"HEAD", HTTP_HEAD}};
    method_ = methods.count(method) ? methods.at(method) : HTTP_GET;
    headers_.clear();
    args_.clear();
    size_t contentLen = 0;
    std::string ctype;
    while (std::getline(ss, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        size_t c = line.find(':');
        if (c == std::string::npos) continue;
        std::string k = line.substr(0, c), v = line.substr(c + 1);
        while (!v.empty() && v[0] == ' ') v.erase(0, 1);
        headers_.push_back({k, v});
        if (sim::lower(k) == "content-length") contentLen = (size_t)atol(v.c_str());
        if (sim::lower(k) == "content-type") ctype = v;
    }
    size_t q = target.find('?');
    uri_ = sim::url_decode(target.substr(0, q));
    auto parseArgs = [&](const std::string& s) {
        std::stringstream as(s);
        std::string kv;
        while (std::getline(as, kv, '&')) {
            if (kv.empty()) continue;
            size_t e = kv.find('=');
            args_.push_back({sim::url_decode(kv.substr(0, e)), e == std::string::npos ? "" : sim::url_decode(kv.substr(e + 1))});
        }
    };
    std::string search = q == std::string::npos ? "" : target.substr(q + 1);
    if (method_ == HTTP_POST || method_ == HTTP_PUT || method_ == HTTP_PATCH || method_ == HTTP_DELETE) {
        while (body.size() < contentLen) {
            double left = HTTP_MAX_POST_WAIT - (sim::now_ms() - start);
            if (left <= 0) return false;
            int n = sim::recv_timeout(fd, tmp, sizeof tmp, left);
            if (n <= 0) return false;
            body.append(tmp, n);
        }
        body.resize(contentLen);
        bool isEncoded = ctype.rfind("application/x-www-form-urlencoded", 0) == 0;
        bool isForm = ctype.rfind("multipart/", 0) == 0;
        if (!isForm && contentLen > 0) {
            if (isEncoded) { if (!search.empty()) search += '&'; search += body; }
            parseArgs(search);
            if (!isEncoded) args_.push_back({"plain", body});
        } else {
            parseArgs(search);
        }
    } else {
        parseArgs(search);
    }
    return true;
}

String WebServer::arg(const String& name) const {
    for (auto& a : args_) if (a.first == name.std()) return String(a.second);
    return String();
}
bool WebServer::hasArg(const String& name) const {
    for (auto& a : args_) if (a.first == name.std()) return true;
    return false;
}
String WebServer::header(const String& name) const {
    for (auto& h : headers_) if (sim::lower(h.first) == sim::lower(name.std())) return String(h.second);
    return String();
}
bool WebServer::hasHeader(const String& name) const {
    for (auto& h : headers_) if (sim::lower(h.first) == sim::lower(name.std())) return true;
    return false;
}
bool WebServer::authenticate(const char* user, const char* pass) {
    std::string h = header("Authorization").std();
    if (h.rfind("Basic ", 0) != 0) return false;
    return sim::b64dec(h.substr(6)) == std::string(user) + ":" + pass;
}
void WebServer::requestAuthentication(HTTPAuthMethod, const char* realm, const String& failMsg) {
    sendHeader("WWW-Authenticate", String("Basic realm=\"") + (realm ? realm : "Login Required") + "\"");
    send(401, "text/html", failMsg);
}
void WebServer::sendHeader(const String& name, const String& value, bool first) {
    std::string h = name.std() + ": " + value.std();
    if (first) respHeaders_.insert(respHeaders_.begin(), h);
    else respHeaders_.push_back(h);
}
void WebServer::send(int code, const char* type, const String& content) {
    if (responded_) { body_ += content.std(); return; }
    responded_ = true;
    code_ = code;
    type_ = type ? type : "text/html";
    body_ += content.std();
}
void WebServer::flushResponse(int fd) {
    if (!responded_) return;  // Handler hat nichts gesendet -> Verbindung wird ohne Antwort geschlossen
    static const std::map<int, const char*> texts = {{200, "OK"}, {204, "No Content"}, {301, "Moved Permanently"}, {302, "Found"},
        {400, "Bad Request"}, {401, "Unauthorized"}, {403, "Forbidden"}, {404, "Not Found"}, {405, "Method Not Allowed"},
        {409, "Conflict"}, {429, "Too Many Requests"}, {500, "Internal Server Error"}, {502, "Bad Gateway"}, {503, "Service Unavailable"}, {504, "Gateway Timeout"}};
    std::string out = "HTTP/1.1 " + std::to_string(code_) + " " + (texts.count(code_) ? texts.at(code_) : "") + "\r\n";
    out += "Content-Type: " + type_ + "\r\n";
    out += "Content-Length: " + std::to_string(body_.size()) + "\r\n";
    if (cors_) out += "Access-Control-Allow-Origin: *\r\n";
    for (auto& h : respHeaders_) out += h + "\r\n";
    out += "Connection: close\r\n\r\n";
    out += body_;
    sim::send_all(fd, out, HTTP_MAX_SEND_WAIT);
    shutdown(fd, SHUT_WR);
}

// =====================================================================================
// mDNS
// =====================================================================================
static std::string svc_key(const char* service, const char* proto) {
    std::string s = service, p = proto;
    if (s[0] != '_') s = "_" + s;
    if (p[0] != '_') p = "_" + p;
    return s + "." + p;
}
bool MDNSResponder::begin(const String& hostname) {
    std::string h = hostname.std();
    // ESP-IDF mdns loest Namenskonflikte durch Anhaengen von "-2", "-3", ... auf.
    std::string cand = h;
    for (int n = 2;; n++) {
        std::string rec = sim::read_file(sim::state_dir() + "/mdns/" + cand + ".rec", "");
        int pid = 0;
        std::stringstream ss(rec);
        std::string line;
        while (std::getline(ss, line)) if (line.rfind("pid=", 0) == 0) pid = atoi(line.c_str() + 4);
        if (rec.empty() || pid == getpid() || !sim::process_alive(pid)) break;
        cand = h + "-" + std::to_string(n);
    }
    if (cand != h) sim::event("mdns_conflict " + h + " -> " + cand);
    host_ = cand;
    started_ = true;
    persist();
    return true;
}
void MDNSResponder::end() {
    if (started_) unlink((sim::state_dir() + "/mdns/" + host_ + ".rec").c_str());
    started_ = false;
}
bool MDNSResponder::addService(const char* service, const char* proto, uint16_t port) {
    services_.push_back(svc_key(service, proto) + ":" + std::to_string(port));
    persist();
    return true;
}
bool MDNSResponder::addServiceTxt(const char* service, const char* proto, const char* key, const char* value) {
    txts_.push_back(svc_key(service, proto) + ":" + key + "=" + value);
    persist();
    return true;
}
void MDNSResponder::persist() {
    if (!started_) return;
    std::string rec = "pid=" + std::to_string(getpid()) + "\nip=" + sim::node_ip() + "\nhost=" + host_ + "\n";
    for (auto& s : services_) rec += "svc=" + s + "\n";
    for (auto& t : txts_) rec += "txt=" + t + "\n";
    sim::write_file(sim::state_dir() + "/mdns/" + host_ + ".rec", rec);
}
int MDNSResponder::queryService(const char* service, const char* proto) {
    results_.clear();
    std::string key = svc_key(service, proto);
    // mdns_query_ptr(..., 3000 ms, max 20) wartet den vollen Timeout ab.
    sim::sleep_sim_ms(atof(sim::env("SIM_MDNS_QUERY_MS", "3000").c_str()));
    if (!sim::wifi_link_up()) return 0;
    DIR* d = opendir((sim::state_dir() + "/mdns").c_str());
    if (!d) return 0;
    std::vector<std::string> files;
    while (dirent* e = readdir(d)) {
        std::string n = e->d_name;
        if (n.size() > 4 && n.substr(n.size() - 4) == ".rec") files.push_back(n);
    }
    closedir(d);
    std::sort(files.begin(), files.end());
    for (auto& f : files) {
        std::string rec = sim::read_file(sim::state_dir() + "/mdns/" + f, "");
        Result r;
        r.port = 0;
        int pid = 0;
        bool has = false;
        std::stringstream ss(rec);
        std::string line;
        while (std::getline(ss, line)) {
            if (line.rfind("pid=", 0) == 0) pid = atoi(line.c_str() + 4);
            else if (line.rfind("ip=", 0) == 0) r.ip = line.substr(3);
            else if (line.rfind("host=", 0) == 0) r.host = line.substr(5);
            else if (line.rfind("svc=" + key + ":", 0) == 0) { has = true; r.port = atoi(line.c_str() + 5 + key.size()); }
            else if (line.rfind("txt=" + key + ":", 0) == 0) {
                std::string kv = line.substr(5 + key.size());
                size_t e = kv.find('=');
                r.txt.push_back({kv.substr(0, e), e == std::string::npos ? "" : kv.substr(e + 1)});
            }
        }
        // Eigene Pakete ignoriert ESP-IDF mdns; tote/offline Knoten antworten nicht.
        if (!has || pid == getpid() || r.ip == sim::node_ip() || !sim::process_alive(pid) || !sim::node_reachable(r.ip)) continue;
        results_.push_back(r);
        if (results_.size() >= 20) break;
    }
    return (int)results_.size();
}
String MDNSResponder::hostname(int i) { return i < (int)results_.size() ? String(results_[i].host) : String(); }
IPAddress MDNSResponder::IP(int i) {
    IPAddress ip;
    if (i < (int)results_.size()) ip.fromString(results_[i].ip.c_str());
    return ip;
}
uint16_t MDNSResponder::port(int i) { return i < (int)results_.size() ? (uint16_t)results_[i].port : 0; }
int MDNSResponder::numTxt(int i) { return i < (int)results_.size() ? (int)results_[i].txt.size() : 0; }
bool MDNSResponder::hasTxt(int i, const char* key) {
    if (i >= (int)results_.size()) return false;
    for (auto& t : results_[i].txt) if (t.first == key) return true;
    return false;
}
String MDNSResponder::txt(int i, const char* key) {
    if (i >= (int)results_.size()) return String();
    for (auto& t : results_[i].txt) if (t.first == key) return String(t.second);
    return String();
}
String MDNSResponder::txt(int i, int t) { return i < (int)results_.size() && t < (int)results_[i].txt.size() ? String(results_[i].txt[t].second) : String(); }
String MDNSResponder::txtKey(int i, int t) { return i < (int)results_.size() && t < (int)results_[i].txt.size() ? String(results_[i].txt[t].first) : String(); }

// =====================================================================================
// HTTPClient
// =====================================================================================
bool HTTPClient::begin(const String& url) {
    std::string u = url.std();
    if (u.rfind("http://", 0) != 0) return false;
    u = u.substr(7);
    size_t slash = u.find('/');
    std::string hostport = u.substr(0, slash);
    uri_ = slash == std::string::npos ? "/" : u.substr(slash);
    size_t colon = hostport.find(':');
    host_ = hostport.substr(0, colon);
    port_ = colon == std::string::npos ? 80 : atoi(hostport.c_str() + colon + 1);
    headers_.clear();
    body_.clear();
    return true;
}
bool HTTPClient::begin(const String& host, uint16_t port, const String& uri) {
    host_ = host.std();
    port_ = port;
    uri_ = uri.std();
    headers_.clear();
    body_.clear();
    return true;
}
void HTTPClient::end() {}
void HTTPClient::setAuthorization(const char* user, const char* pass) {
    std::string cred = std::string(user) + ":" + pass;
    headers_.push_back("Authorization: Basic " + sim::b64enc((const unsigned char*)cred.data(), cred.size()));
}
void HTTPClient::addHeader(const String& name, const String& value, bool, bool) { headers_.push_back(name.std() + ": " + value.std()); }
int HTTPClient::sendRequest(const char* method, const std::string& payload) {
    body_.clear();
    int fd = sim::tcp_connect(host_, port_, (double)connectTimeout_);
    if (fd < 0) return HTTPC_ERROR_CONNECTION_REFUSED;
    std::string req = std::string(method) + " " + uri_ + " HTTP/1.1\r\nHost: " + host_ + "\r\nUser-Agent: ESP32HTTPClient\r\nConnection: close\r\n";
    for (auto& h : headers_) req += h + "\r\n";
    if (!payload.empty() || strcmp(method, "POST") == 0 || strcmp(method, "PUT") == 0) req += "Content-Length: " + std::to_string(payload.size()) + "\r\n";
    req += "\r\n" + payload;
    if (!sim::send_all(fd, req, readTimeout_)) { close(fd); return HTTPC_ERROR_SEND_HEADER_FAILED; }
    std::string resp;
    char tmp[4096];
    size_t hdrEnd;
    while ((hdrEnd = resp.find("\r\n\r\n")) == std::string::npos) {
        int n = sim::recv_timeout(fd, tmp, sizeof tmp, readTimeout_);
        if (n < 0) { close(fd); return HTTPC_ERROR_READ_TIMEOUT; }
        if (n == 0) { close(fd); return resp.empty() ? HTTPC_ERROR_CONNECTION_LOST : HTTPC_ERROR_NO_HTTP_SERVER; }
        resp.append(tmp, n);
    }
    int code = 0;
    if (sscanf(resp.c_str(), "HTTP/%*s %d", &code) != 1) { close(fd); return HTTPC_ERROR_NO_HTTP_SERVER; }
    std::string head = sim::lower(resp.substr(0, hdrEnd));
    body_ = resp.substr(hdrEnd + 4);
    long clen = -1;
    size_t cl = head.find("content-length:");
    if (cl != std::string::npos) clen = atol(head.c_str() + cl + 15);
    while (clen < 0 || (long)body_.size() < clen) {
        int n = sim::recv_timeout(fd, tmp, sizeof tmp, readTimeout_);
        if (n <= 0) break;
        body_.append(tmp, n);
    }
    if (clen >= 0 && (long)body_.size() > clen) body_.resize(clen);
    close(fd);
    return code;
}
String HTTPClient::errorToString(int e) {
    switch (e) {
        case HTTPC_ERROR_CONNECTION_REFUSED: return String("connection refused");
        case HTTPC_ERROR_SEND_HEADER_FAILED: return String("send header failed");
        case HTTPC_ERROR_CONNECTION_LOST: return String("connection lost");
        case HTTPC_ERROR_NO_HTTP_SERVER: return String("no HTTP server");
        case HTTPC_ERROR_READ_TIMEOUT: return String("read Timeout");
        default: return String();
    }
}

// =====================================================================================
// WebSocketsServer
// =====================================================================================
WebSocketsServer::~WebSocketsServer() { close(); }
void WebSocketsServer::begin() {
    if (listenFd_ < 0) listenFd_ = sim::listen_on(sim::node_ip(), port_);
    sim::event("websocket_begin " + std::to_string(port_));
}
void WebSocketsServer::close() {
    for (int i = 0; i < WEBSOCKETS_SERVER_CLIENT_MAX; i++) dropClient(i, false);
    if (listenFd_ >= 0) { ::close(listenFd_); listenFd_ = -1; }
}
IPAddress WebSocketsServer::remoteIP(uint8_t num) { return num < WEBSOCKETS_SERVER_CLIENT_MAX ? clients_[num].ip : IPAddress(); }
int WebSocketsServer::connectedClients(bool) {
    int n = 0;
    for (auto& c : clients_) if (c.state == 2) n++;
    return n;
}
void WebSocketsServer::enableHeartbeat(uint32_t pingInterval, uint32_t pongTimeout, uint8_t count) {
    hbInterval_ = pingInterval;
    hbTimeout_ = pongTimeout;
    hbCount_ = count;
}
void WebSocketsServer::dropClient(uint8_t num, bool notify) {
    Client& c = clients_[num];
    if (c.fd < 0) return;
    bool wasConnected = c.state == 2;
    ::close(c.fd);
    c = Client();
    if (notify && wasConnected && cb_) cb_(num, WStype_DISCONNECTED, nullptr, 0);
}
void WebSocketsServer::disconnect() { for (int i = 0; i < WEBSOCKETS_SERVER_CLIENT_MAX; i++) dropClient(i, true); }
void WebSocketsServer::disconnect(uint8_t num) { if (num < WEBSOCKETS_SERVER_CLIENT_MAX) dropClient(num, true); }

bool WebSocketsServer::sendFrame(uint8_t num, uint8_t opcode, const uint8_t* data, size_t len) {
    if (num >= WEBSOCKETS_SERVER_CLIENT_MAX || clients_[num].state != 2) return false;
    if (!sim::wifi_link_up()) return true;  // Daten gehen im Funkloch verloren
    std::string f;
    f += (char)(0x80 | opcode);
    if (len < 126) f += (char)len;
    else if (len < 65536) { f += (char)126; f += (char)(len >> 8); f += (char)(len & 255); }
    else { f += (char)127; for (int i = 7; i >= 0; i--) f += (char)((uint64_t)len >> (8 * i)); }
    if (len) f.append((const char*)data, len);
    // Wie die Library: blockierendes Schreiben mit WEBSOCKETS_TCP_TIMEOUT
    if (!sim::send_all(clients_[num].fd, f, WEBSOCKETS_TCP_TIMEOUT)) { dropClient(num, true); return false; }
    return true;
}
bool WebSocketsServer::sendTXT(uint8_t num, const uint8_t* payload, size_t length, bool) {
    if (!length) length = strlen((const char*)payload);
    return sendFrame(num, 0x1, payload, length);
}
bool WebSocketsServer::broadcastTXT(const uint8_t* payload, size_t length, bool) {
    if (!length) length = strlen((const char*)payload);
    bool ok = true;
    for (int i = 0; i < WEBSOCKETS_SERVER_CLIENT_MAX; i++)
        if (clients_[i].state == 2) ok &= sendFrame(i, 0x1, payload, length);
    return ok;
}
void WebSocketsServer::handleHeader(uint8_t num) {
    Client& c = clients_[num];
    size_t end = c.in.find("\r\n\r\n");
    if (end == std::string::npos) return;
    std::string head = c.in.substr(0, end);
    c.in.erase(0, end + 4);
    std::string url = "/", key;
    std::stringstream ss(head);
    std::string line;
    std::getline(ss, line);
    { std::stringstream rl(line); std::string m; rl >> m >> url; }
    bool upgrade = false;
    std::vector<std::pair<std::string, std::string>> hdrs;
    while (std::getline(ss, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        size_t col = line.find(':');
        if (col == std::string::npos) continue;
        std::string k = sim::lower(line.substr(0, col)), v = line.substr(col + 1);
        while (!v.empty() && v[0] == ' ') v.erase(0, 1);
        hdrs.push_back({line.substr(0, col), v});
        if (k == "sec-websocket-key") key = v;
        if (k == "upgrade" && sim::lower(v) == "websocket") upgrade = true;
    }
    if (validate_) {
        for (auto& h : hdrs) {
            if (!validate_(String(h.first), String(h.second))) {
                sim::send_all(c.fd, "HTTP/1.1 403 Forbidden\r\nConnection: close\r\n\r\n", 1000);
                dropClient(num, false);
                return;
            }
        }
    }
    if (!upgrade || key.empty()) {
        sim::send_all(c.fd, "HTTP/1.1 400 Bad Request\r\nConnection: close\r\n\r\n", 1000);
        dropClient(num, false);
        return;
    }
    unsigned char dig[20];
    sim::sha1(key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11", dig);
    std::string resp = "HTTP/1.1 101 Switching Protocols\r\nServer: arduino-WebSocketsServer\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                       "Sec-WebSocket-Version: 13\r\nSec-WebSocket-Accept: " + sim::b64enc(dig, 20) + "\r\n\r\n";
    if (!sim::send_all(c.fd, resp, WEBSOCKETS_TCP_TIMEOUT)) { dropClient(num, false); return; }
    c.state = 2;
    c.lastPing = c.lastPong = sim::now_ms();
    if (cb_) cb_(num, WStype_CONNECTED, (uint8_t*)url.c_str(), url.size());
}
void WebSocketsServer::handleFrames(uint8_t num) {
    for (;;) {
        Client& c = clients_[num];
        if (c.state != 2 || c.in.size() < 2) return;
        const unsigned char* p = (const unsigned char*)c.in.data();
        uint8_t op = p[0] & 0x0F;
        bool masked = p[1] & 0x80;
        uint64_t len = p[1] & 0x7F;
        size_t off = 2;
        if (len == 126) { if (c.in.size() < 4) return; len = (uint64_t)p[2] << 8 | p[3]; off = 4; }
        else if (len == 127) { if (c.in.size() < 10) return; len = 0; for (int i = 0; i < 8; i++) len = len << 8 | p[2 + i]; off = 10; }
        size_t need = off + (masked ? 4 : 0) + (size_t)len;
        if (c.in.size() < need) return;
        std::string payload = c.in.substr(off + (masked ? 4 : 0), (size_t)len);
        if (masked) for (size_t i = 0; i < payload.size(); i++) payload[i] ^= p[off + (i % 4)];
        c.in.erase(0, need);
        if (op == 0x1 || op == 0x2) {
            if (cb_) cb_(num, op == 1 ? WStype_TEXT : WStype_BIN, (uint8_t*)payload.c_str(), payload.size());
        } else if (op == 0x8) {
            sendFrame(num, 0x8, (const uint8_t*)payload.data(), payload.size() >= 2 ? 2 : 0);
            dropClient(num, true);
            return;
        } else if (op == 0x9) {
            sendFrame(num, 0xA, (const uint8_t*)payload.data(), payload.size());
            if (cb_) cb_(num, WStype_PING, (uint8_t*)payload.c_str(), payload.size());
        } else if (op == 0xA) {
            c.pingPending = false;
            c.missedPongs = 0;
            c.lastPong = sim::now_ms();
            if (cb_) cb_(num, WStype_PONG, (uint8_t*)payload.c_str(), payload.size());
        }
    }
}
void WebSocketsServer::loop() {
    if (listenFd_ < 0 || !sim::wifi_link_up()) return;
    for (;;) {
        int fd = accept(listenFd_, nullptr, nullptr);
        if (fd < 0) break;
        int slot = -1;
        for (int i = 0; i < WEBSOCKETS_SERVER_CLIENT_MAX; i++) if (clients_[i].fd < 0) { slot = i; break; }
        if (slot < 0) { sim::event("websocket_reject_full"); ::close(fd); continue; }
        fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
        clients_[slot].fd = fd;
        clients_[slot].state = 1;
        clients_[slot].ip = sim::peer_ip(fd);
    }
    char buf[4096];
    for (int i = 0; i < WEBSOCKETS_SERVER_CLIENT_MAX; i++) {
        if (clients_[i].fd < 0) continue;
        for (;;) {
            ssize_t n = recv(clients_[i].fd, buf, sizeof buf, MSG_DONTWAIT);
            if (n > 0) { clients_[i].in.append(buf, n); continue; }
            if (n == 0 || (errno != EAGAIN && errno != EWOULDBLOCK)) { dropClient(i, true); }
            break;
        }
        if (clients_[i].state == 1) handleHeader(i);
        if (clients_[i].state == 2) handleFrames(i);
        if (clients_[i].state == 2 && hbInterval_) {
            Client& c = clients_[i];
            double now = sim::now_ms();
            if (c.pingPending && now - c.lastPing > hbTimeout_) {
                c.pingPending = false;
                if (++c.missedPongs >= hbCount_) { dropClient(i, true); continue; }
            }
            if (!c.pingPending && now - c.lastPing > hbInterval_) {
                c.lastPing = now;
                c.pingPending = true;
                sendPing(i);
            }
        }
    }
}

// =====================================================================================
// main(): setup() + loop() mit Metriken
// =====================================================================================
void setup();
void loop();

int main(int argc, char** argv) {
    sim::init(argc, argv);
    g_reset_reason = (esp_reset_reason_t)atoi(sim::env("SIM_RESET_REASON", "1").c_str());
    g_loop_thread = std::this_thread::get_id();
    g_heap_baseline = mallinfo2().uordblks;
    std::thread(wifi_thread).detach();
    std::thread(wdt_thread).detach();
    sim::event("boot " + std::to_string((int)g_reset_reason));
    sim::write_file(sim::node_dir() + "/metrics", "phase=setup\n");
    setup();
    sim::event("setup_done");
    unsigned long loops = 0;
    double maxLoop = 0, lastWrite = 0, bootMs = sim::now_ms();
    for (;;) {
        double t = sim::now_ms();
        loop();
        double d = sim::now_ms() - t;
        loops++;
        if (d > maxLoop) maxLoop = d;
        feedLoopWDT();
        if (d > 1000) sim::event("slow_loop " + std::to_string((long)d));
        if (sim::now_ms() - lastWrite > 100 * atof(sim::env("SIM_TIME_SCALE", "1").c_str())) {
            lastWrite = sim::now_ms();
            char buf[256];
            snprintf(buf, sizeof buf, "phase=loop\nloops=%lu\nmax_loop_ms=%.0f\nlast_loop_ms=%.0f\nuptime_ms=%.0f\nfree_heap=%u\n",
                     loops, maxLoop, d, sim::now_ms() - bootMs, ESP.getFreeHeap());
            sim::write_file(sim::node_dir() + "/metrics", buf);
        }
        std::this_thread::sleep_for(std::chrono::microseconds(500));
    }
}
