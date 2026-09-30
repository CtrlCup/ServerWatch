// Minimaler Nachbau von mbedtls/md.h (nur HMAC-SHA256 als One-Shot), wie im ESP32-Core enthalten.
#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>

typedef enum { MBEDTLS_MD_NONE = 0, MBEDTLS_MD_SHA256 = 6 } mbedtls_md_type_t;
typedef struct mbedtls_md_info_t { mbedtls_md_type_t type; } mbedtls_md_info_t;

namespace sim_sha256 {
struct Ctx { uint32_t h[8]; uint8_t buf[64]; uint64_t len; size_t fill; };
inline uint32_t ror(uint32_t v, int b) { return (v >> b) | (v << (32 - b)); }
inline void block(Ctx& c, const uint8_t* p) {
    static const uint32_t k[64] = {
        0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01,
        0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc,
        0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147,
        0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
        0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116, 0x1e376c08,
        0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
        0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
    uint32_t w[64];
    for (int i = 0; i < 16; i++) w[i] = (uint32_t)p[4 * i] << 24 | (uint32_t)p[4 * i + 1] << 16 | (uint32_t)p[4 * i + 2] << 8 | p[4 * i + 3];
    for (int i = 16; i < 64; i++) {
        uint32_t s0 = ror(w[i - 15], 7) ^ ror(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = ror(w[i - 2], 17) ^ ror(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    uint32_t a = c.h[0], b = c.h[1], cc = c.h[2], d = c.h[3], e = c.h[4], f = c.h[5], g = c.h[6], h = c.h[7];
    for (int i = 0; i < 64; i++) {
        uint32_t t1 = h + (ror(e, 6) ^ ror(e, 11) ^ ror(e, 25)) + ((e & f) ^ (~e & g)) + k[i] + w[i];
        uint32_t t2 = (ror(a, 2) ^ ror(a, 13) ^ ror(a, 22)) + ((a & b) ^ (a & cc) ^ (b & cc));
        h = g; g = f; f = e; e = d + t1; d = cc; cc = b; b = a; a = t1 + t2;
    }
    c.h[0] += a; c.h[1] += b; c.h[2] += cc; c.h[3] += d; c.h[4] += e; c.h[5] += f; c.h[6] += g; c.h[7] += h;
}
inline void init(Ctx& c) {
    static const uint32_t iv[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    memcpy(c.h, iv, sizeof iv);
    c.len = 0;
    c.fill = 0;
}
inline void update(Ctx& c, const uint8_t* d, size_t n) {
    for (size_t i = 0; i < n; i++) {
        c.buf[c.fill++] = d[i];
        c.len++;
        if (c.fill == 64) { block(c, c.buf); c.fill = 0; }
    }
}
inline void finish(Ctx& c, uint8_t out[32]) {
    uint64_t bits = c.len * 8;
    uint8_t pad = 0x80;
    update(c, &pad, 1);
    uint8_t zero = 0;
    while (c.fill != 56) update(c, &zero, 1);
    for (int i = 7; i >= 0; i--) { uint8_t b = (uint8_t)(bits >> (8 * i)); update(c, &b, 1); }
    for (int i = 0; i < 8; i++) for (int j = 0; j < 4; j++) out[4 * i + j] = (uint8_t)(c.h[i] >> (24 - 8 * j));
}
}  // namespace sim_sha256

inline const mbedtls_md_info_t* mbedtls_md_info_from_type(mbedtls_md_type_t type) {
    static const mbedtls_md_info_t sha256 = {MBEDTLS_MD_SHA256};
    return type == MBEDTLS_MD_SHA256 ? &sha256 : nullptr;
}

inline int mbedtls_md_hmac(const mbedtls_md_info_t* info, const unsigned char* key, size_t keylen,
                           const unsigned char* input, size_t ilen, unsigned char* output) {
    if (!info || info->type != MBEDTLS_MD_SHA256) return -1;
    uint8_t k[64] = {0};
    if (keylen > 64) {
        sim_sha256::Ctx c;
        sim_sha256::init(c);
        sim_sha256::update(c, key, keylen);
        sim_sha256::finish(c, k);
    } else {
        memcpy(k, key, keylen);
    }
    uint8_t ipad[64], opad[64], inner[32];
    for (int i = 0; i < 64; i++) { ipad[i] = k[i] ^ 0x36; opad[i] = k[i] ^ 0x5c; }
    sim_sha256::Ctx c;
    sim_sha256::init(c);
    sim_sha256::update(c, ipad, 64);
    sim_sha256::update(c, input, ilen);
    sim_sha256::finish(c, inner);
    sim_sha256::init(c);
    sim_sha256::update(c, opad, 64);
    sim_sha256::update(c, inner, 32);
    sim_sha256::finish(c, output);
    return 0;
}
