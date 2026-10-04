// Attrappe von Preferences (NVS): eine Datei pro Schluessel unter nodes/<ip>/nvs/<ns>/<key>.
// Ueberlebt ESP.restart() und kill/start. Jeder put... schreibt das Ereignis `nvs_write <ns>/<key>`.
// Dateiformat: 1. Byte = Typ-Tag (wie die Typen im echten NVS), danach die Nutzdaten: Zahlen little-endian
// in ihrer Groesse, Bool wie im Original als uint8, String mit abschliessendem NUL, Blob roh.
// Wie im Original: Namespace und Schluessel max. 15 Zeichen; Typ- oder Groessenabweichung -> Standardwert
// (getBytes/getBytesLength -> 0). Zwischen putUChar und putBool gibt es keinen Unterschied (beides U8).
// Die Tags stehen auch in test/tests/simnet.py (NVS_TYPES); nvs_raw liefert nur die Nutzdaten.
#pragma once
#include "Arduino.h"

class Preferences {
public:
    enum : uint8_t { T_ANY = 0, T_U8 = 1, T_I32 = 2, T_U32 = 3, T_STR = 0x21, T_BLOB = 0x42 };
    bool begin(const char* name, bool readOnly = false, const char* partition_label = nullptr);
    void end();
    bool clear();
    bool remove(const char* key);
    bool isKey(const char* key);

    size_t putUChar(const char* key, uint8_t v) { return putRaw(key, T_U8, &v, sizeof v); }
    size_t putUInt(const char* key, uint32_t v) { return putRaw(key, T_U32, &v, sizeof v); }
    size_t putInt(const char* key, int32_t v) { return putRaw(key, T_I32, &v, sizeof v); }
    size_t putBool(const char* key, bool v) { uint8_t b = v ? 1 : 0; return putRaw(key, T_U8, &b, 1); }
    size_t putString(const char* key, const char* v) { return putRaw(key, T_STR, v, strlen(v) + 1) ? strlen(v) : 0; }
    size_t putString(const char* key, const String& v) { return putString(key, v.c_str()); }
    size_t putBytes(const char* key, const void* v, size_t len) { return putRaw(key, T_BLOB, v, len); }

    uint8_t getUChar(const char* key, uint8_t def = 0) { return getFixed<uint8_t>(key, T_U8, def); }
    uint32_t getUInt(const char* key, uint32_t def = 0) { return getFixed<uint32_t>(key, T_U32, def); }
    int32_t getInt(const char* key, int32_t def = 0) { return getFixed<int32_t>(key, T_I32, def); }
    bool getBool(const char* key, bool def = false) { return getFixed<uint8_t>(key, T_U8, def ? 1 : 0) != 0; }
    String getString(const char* key, String def = String());
    size_t getString(const char* key, char* out, size_t maxLen);
    size_t getBytesLength(const char* key);
    size_t getBytes(const char* key, void* out, size_t maxLen);

private:
    bool readRaw(const char* key, std::string& out, uint8_t type);
    size_t putRaw(const char* key, uint8_t type, const void* data, size_t len);
    template <typename T> T getFixed(const char* key, uint8_t type, T def) {
        std::string raw;
        if (!readRaw(key, raw, type) || raw.size() != sizeof(T)) return def;
        T v;
        memcpy(&v, raw.data(), sizeof v);
        return v;
    }
    std::string path(const char* key) const;
    bool started_ = false, readOnly_ = false;
    std::string ns_;
};
