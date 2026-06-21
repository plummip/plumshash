#include <stdint.h>
#include <string.h>
static inline uint64_t rotl64(uint64_t x, int k) { return (x<<k)|(x>>(64-k)); }
#define PHI 0x9E3779B97F4A7C15ULL
#define M1 0x85EBCA77C2B2AE3DULL
#define M2 0xBF58476D1CE4E5B9ULL
#define M3 0x94D049BB133111EBULL

/* v2 baseline */
uint64_t v2(const void *b, size_t l, uint64_t s) {
    const uint8_t *p = (const uint8_t*)b, *e = p + l;
    uint64_t ba = s ^ (l * PHI), h1 = ba * PHI, h2 = ba * M1, h3 = ba * M2, h4 = ba * M3;
    while (p + 32 <= e) {
        uint64_t v; memcpy(&v, p, 8); h1 ^= rotl64(v, 11); h1 *= PHI; p += 8;
        memcpy(&v, p, 8); h2 ^= rotl64(v, 19); h2 *= PHI; p += 8;
        memcpy(&v, p, 8); h3 ^= rotl64(v, 35); h3 *= PHI; p += 8;
        memcpy(&v, p, 8); h4 ^= rotl64(v, 47); h4 *= PHI; p += 8;
    }
    while (p + 8 <= e) { uint64_t v; memcpy(&v, p, 8); h1 ^= rotl64(v, 11); h1 *= PHI; p += 8; }
    uint64_t t = 0;
    switch (e - p) {
        case 7: t ^= (uint64_t)p[6] << 48; case 6: t ^= (uint64_t)p[5] << 40;
        case 5: t ^= (uint64_t)p[4] << 32; case 4: t ^= (uint64_t)p[3] << 24;
        case 3: t ^= (uint64_t)p[2] << 16; case 2: t ^= (uint64_t)p[1] << 8;
        case 1: t ^= p[0]; h1 ^= rotl64(t, 11); h1 *= PHI;
    }
    h1 ^= h2; h3 ^= h4; h1 ^= h3;
    h1 ^= h1 >> 29; h1 *= M1; h1 ^= h1 >> 31; h1 *= M2; h1 ^= h1 >> 37; h1 *= PHI; h1 ^= h1 >> 41;
    return h1;
}

/* v4x: 4x unrolled, direct loads, __builtin_prefetch */
uint64_t v4x(const void *b, size_t l, uint64_t s) {
    const uint8_t *p = (const uint8_t*)b, *e = p + l;
    uint64_t ba = s ^ (l * PHI), h1 = ba * PHI, h2 = ba * M1, h3 = ba * M2, h4 = ba * M3;
    while (p + 128 <= e) {
        __builtin_prefetch(p + 256, 0, 3);
        h1 ^= rotl64(*(const uint64_t*)(p), 11); h1 *= PHI; p += 8;
        h2 ^= rotl64(*(const uint64_t*)(p), 19); h2 *= PHI; p += 8;
        h3 ^= rotl64(*(const uint64_t*)(p), 35); h3 *= PHI; p += 8;
        h4 ^= rotl64(*(const uint64_t*)(p), 47); h4 *= PHI; p += 8;
        h1 ^= rotl64(*(const uint64_t*)(p), 11); h1 *= PHI; p += 8;
        h2 ^= rotl64(*(const uint64_t*)(p), 19); h2 *= PHI; p += 8;
        h3 ^= rotl64(*(const uint64_t*)(p), 35); h3 *= PHI; p += 8;
        h4 ^= rotl64(*(const uint64_t*)(p), 47); h4 *= PHI; p += 8;
        h1 ^= rotl64(*(const uint64_t*)(p), 11); h1 *= PHI; p += 8;
        h2 ^= rotl64(*(const uint64_t*)(p), 19); h2 *= PHI; p += 8;
        h3 ^= rotl64(*(const uint64_t*)(p), 35); h3 *= PHI; p += 8;
        h4 ^= rotl64(*(const uint64_t*)(p), 47); h4 *= PHI; p += 8;
        h1 ^= rotl64(*(const uint64_t*)(p), 11); h1 *= PHI; p += 8;
        h2 ^= rotl64(*(const uint64_t*)(p), 19); h2 *= PHI; p += 8;
        h3 ^= rotl64(*(const uint64_t*)(p), 35); h3 *= PHI; p += 8;
        h4 ^= rotl64(*(const uint64_t*)(p), 47); h4 *= PHI; p += 8;
    }
    while (p + 32 <= e) {
        h1 ^= rotl64(*(const uint64_t*)(p), 11); h1 *= PHI; p += 8;
        h2 ^= rotl64(*(const uint64_t*)(p), 19); h2 *= PHI; p += 8;
        h3 ^= rotl64(*(const uint64_t*)(p), 35); h3 *= PHI; p += 8;
        h4 ^= rotl64(*(const uint64_t*)(p), 47); h4 *= PHI; p += 8;
    }
    while (p + 8 <= e) { h1 ^= rotl64(*(const uint64_t*)(p), 11); h1 *= PHI; p += 8; }
    uint64_t t = 0;
    switch (e - p) {
        case 7: t ^= (uint64_t)p[6] << 48; case 6: t ^= (uint64_t)p[5] << 40;
        case 5: t ^= (uint64_t)p[4] << 32; case 4: t ^= (uint64_t)p[3] << 24;
        case 3: t ^= (uint64_t)p[2] << 16; case 2: t ^= (uint64_t)p[1] << 8;
        case 1: t ^= p[0]; h1 ^= rotl64(t, 11); h1 *= PHI;
    }
    h1 ^= h2; h3 ^= h4; h1 ^= h3;
    h1 ^= h1 >> 29; h1 *= M1; h1 ^= h1 >> 31; h1 *= M2; h1 ^= h1 >> 37; h1 *= PHI; h1 ^= h1 >> 41;
    return h1;
}
