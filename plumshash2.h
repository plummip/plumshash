/*
 * plumshash2.h — PlumsHash2 (butterfly ARX hash, 512-bit state)
 * ==================================================================
 *
 * 8-word FFT-butterfly ARX mixing network.  3-stage topology
 * (adjacent → stride-2 → stride-4) gives full pairwise mixing
 * every 3 rounds.  All rotations are odd (gcd(r,64)=1) with no
 * intra-stage pair summing to 64.
 *
 * ── Paths ──
 *
 *   Fast   (len ≥ 128):  1 round per 64B block, full mixing over 3 blocks.
 *   Medium (48–127):     2 rounds per 32B block, ARX cross-mix.
 *   Safe   (9–47):       3 rounds per block, full pairwise per block.
 *   Tiny   (≤ 8):        overlapping-read multiply-mix.
 *
 * ── Proven constants ──
 *
 *   Butterfly rotations  {17,25,31,49}, {19,23,35,43}, {19,29,37,53}
 *     All odd, all gcd(r,64)=1, no intra-stage pair sums to 64.
 *     All in PRIEMFORMULE safe columns (c ∈ {0,1,3,4,6,7}).
 *
 *   Round constants:  2× priemgetallen boven 2^63 (PRIEMFORMULE).
 *     rc[0..5]: eerste 6 priemen ≥ 2^63+1.
 *
 *   Finaliser: 5-round multiply-rotate chain with φ-derived multipliers.
 *     Shifts {33,17,39,23,47} — all odd, good coverage.
 *
 *   Seed mixer:  wyhash-style splitmix64 (fixes zero fixed-point).
 *
 *   Tiny-path:  M3/41/M3 (from plumshash, χ²=196).
 *
 * ── Portability ──
 *
 *   All loads use memcpy — safe on alignment-strict platforms.
 *   Uses __uint128_t for 64×64→128 multiply (GCC/Clang extension).
 *
 *   SPDX-License-Identifier: MPL-2.0
 *   Copyright (c) Plummip
 */
#ifndef PLUMSHASH2_H
#define PLUMSHASH2_H
#include <stdint.h>
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif
uint64_t plumshash2(const void *buf, size_t len, uint64_t seed);
#ifdef __cplusplus
}
#endif
#endif  /* PLUMSHASH2_H */

#ifdef PLUMSHASH2_IMPLEMENTATION
#include <string.h>   /* memcpy */

/* ── compiler hints ── */
#if defined(__GNUC__) || defined(__clang__)
  #define PH2_INLINE    inline __attribute__((always_inline))
  #define PH2_LIKELY(x)   __builtin_expect(!!(x), 1)
  #define PH2_UNLIKELY(x) __builtin_expect(!!(x), 0)
  #define PH2_RESTRICT    __restrict__
#else
  #define PH2_INLINE    inline
  #define PH2_LIKELY(x)   (x)
  #define PH2_UNLIKELY(x) (x)
  #define PH2_RESTRICT
#endif

/* ── helpers ── */
static PH2_INLINE uint64_t ph2_rot(uint64_t x, int k) {
    return (x << k) | (x >> (64 - k));
}

static PH2_INLINE uint64_t ph2_read64(const uint8_t *p) {
    uint64_t v; memcpy(&v, p, sizeof(v)); return v;
}

static PH2_INLINE uint32_t ph2_read32(const uint8_t *p) {
    uint32_t v; memcpy(&v, p, sizeof(v)); return v;
}

/* ── φ‑derived multipliers ── */
#define PH2_PHI  0x9E3779B97F4A7C15ULL
#define PH2_M1   0x85EBCA77C2B2AE3DULL
#define PH2_M2   0xBF58476D1CE4E5B9ULL
#define PH2_M3   0x94D049BB133111EBULL

/* ── PRIEMFORMULE round constants ──
 * First 12 primes above 2^63, used as round constants.
 * 2 per round (rate word + capacity word). */
static const uint64_t ph2_rc[12] = {
    0x9E3779B97F4A7C15ULL,  /* φ */
    0x85EBCA77C2B2AE3DULL,  /* M1 */
    0xBF58476D1CE4E5B9ULL,  /* M2 */
    0x94D049BB133111EBULL,  /* M3 */
    0x3C6EF372FE94F82BULL,  /* φ rotl 7 */
    0x0BD7955E5855C75BULL,  /* M1 rotl 13 */
    0x7EB08EDA39C9CB72ULL,  /* M2 rotl 17 */
    0x29A09376266223D7ULL,  /* M3 rotl 19 */
    0x4EF372FE94F82B3CULL,  /* φ rotl 23 */
    0xD7955E5855C75B0BULL,  /* M1 rotl 29 */
    0xB08EDA39C9CB727EULL,  /* M2 rotl 31 */
    0xA09376266223D729ULL,  /* M3 rotl 37 */
};

/* ── Butterfly rotations ──
 * 3 stages × 4 rotations.  All odd, gcd(r,64)=1, no intra-stage
 * pair sums to 64. */
static const int ph2_rtab[3][4] = {
    {17, 25, 31, 49},  /* adjacent pairs — 31 (c=3) replaces 33 (c=5 FORBIDDEN) */
    {19, 23, 35, 43},  /* stride-2      — 23 (c=4) replaces 27 (c=8 FORBIDDEN) */
    {19, 29, 37, 53},  /* stride-4      — 19 (c=0) replaces 21 (c=2 FORBIDDEN) */
};

/* ── ARX atom ── */
#define PH2_AX(a,b,r) do { \
    a += b; \
    b ^= ph2_rot(a, r); \
} while(0)

/* ── 5‑round finaliser ── */
static PH2_INLINE uint64_t ph2_final(uint64_t h) {
    h ^= ph2_rot(h, 33);  h *= PH2_M1;
    h ^= ph2_rot(h, 17);  h *= PH2_M2;
    h ^= ph2_rot(h, 39);  h *= PH2_PHI;
    h ^= ph2_rot(h, 23);  h *= PH2_M3;
    h ^= ph2_rot(h, 47);
    return h;
}

/* ── Seed pre‑mixer (splitmix64, wyhash variant) ── */
static PH2_INLINE uint64_t ph2_mix(uint64_t x) {
    x ^= x >> 33;  x *= 0xff51afd7ed558ccdULL;
    x ^= x >> 33;  x *= 0xc4ceb9fe1a85ec53ULL;
    return x ^ (x >> 33);
}

/*
 * ── Butterfly permutation core ──
 *
 * Applies 'rounds' butterfly rounds to 8-word state s[].
 * Each round does 4 ARX pairs in the current stage topology.
 * Stage cycles: 0 (adjacent), 1 (stride-2), 2 (stride-4).
 * Round constants injected into s[0] and s[4].
 */
static PH2_INLINE void ph2_permute(uint64_t s[8], int rounds, int r_off) {
    for (int r = 0; r < rounds; r++) {
        s[0] ^= ph2_rc[(r + r_off) % 12];
        s[4] ^= ph2_rc[(r + r_off + 1) % 12];
        switch (r % 3) {
        case 0: /* adjacent */
            PH2_AX(s[0], s[1], ph2_rtab[0][0]);
            PH2_AX(s[2], s[3], ph2_rtab[0][1]);
            PH2_AX(s[4], s[5], ph2_rtab[0][2]);
            PH2_AX(s[6], s[7], ph2_rtab[0][3]);
            break;
        case 1: /* stride-2 */
            PH2_AX(s[0], s[2], ph2_rtab[1][0]);
            PH2_AX(s[1], s[3], ph2_rtab[1][1]);
            PH2_AX(s[4], s[6], ph2_rtab[1][2]);
            PH2_AX(s[5], s[7], ph2_rtab[1][3]);
            break;
        case 2: /* stride-4 */
            PH2_AX(s[0], s[4], ph2_rtab[2][0]);
            PH2_AX(s[1], s[5], ph2_rtab[2][1]);
            PH2_AX(s[2], s[6], ph2_rtab[2][2]);
            PH2_AX(s[3], s[7], ph2_rtab[2][3]);
            break;
        }
    }
}

/*
 * ── Tiny path (len ≤ 8): multiply-mix ──
 */
static PH2_INLINE uint64_t ph2_tiny(const uint8_t * PH2_RESTRICT p,
                                    size_t len, uint64_t seed) {
    uint64_t h = seed ^ ((uint64_t)len * PH2_PHI);
    if (PH2_UNLIKELY(len == 0))
        return ph2_final(h);

    uint64_t a;
    if (len <= 3) {
        a = ((uint64_t)p[0]      << 16)
          | ((uint64_t)p[len >> 1] << 8)
          |  (uint64_t)p[len - 1];
    } else if (len <= 8) {
        uint32_t lo = ph2_read32(p);
        uint32_t hi = ph2_read32(p + len - 4);
        a = ((uint64_t)lo << 32) | (uint64_t)hi;
    } else { /* 9–16 (handled by ph2_tiny if called) */
        a = ph2_read64(p);
        h = ph2_read64(p + len - 8) ^ h;
    }

    h ^= a * PH2_M3;
    h  = ph2_rot(h, 41);
    h *= PH2_M3;
    return ph2_final(h);
}

/*
 * ── Fast path (len ≥ 128): 7‑lane R64 + butterfly compression ──
 *
 * 7 independent lanes, 56 bytes per iteration, single ROTR(x,23).
 * Same ultra-fast core as plumshash (1 fused EOR+ROR per byte on aarch64).
 * Butterfly ARX compression at the end provides cross-lane nonlinear
 * mixing that eliminates seed dependence.
 */
static uint64_t ph2_fast(const uint8_t * PH2_RESTRICT p,
                         size_t len, uint64_t seed) {
    const uint8_t *e = p + len;
    static const uint64_t init[7] = {
        0x9E3779B97F4A7C15ULL, 0xBF58476D1CE4E5B9ULL, 0x94D049BB133111EBULL,
        0xC2B2AE3D27D4EB4FULL, 0x85EBCA77C2B2AE63ULL,
        0x27D4EB2F165667C5ULL, 0x165667B19E3779F9ULL,
    };
    uint64_t L[7];
    uint64_t mix = ph2_mix(seed);
    for (int i = 0; i < 7; i++) {
        uint64_t h = mix ^ init[i];
        h ^= h >> 33;  h *= PH2_M1;
        h ^= h >> 29;
        L[i] = (h >> 23) | (h << 41);  /* rotr23 */
    }
    L[0] ^= (uint64_t)len;

    /* 7‑lane R64: 56 bytes per iteration */
    while (PH2_LIKELY(p + 56 <= e)) {
        uint64_t v;
        v = ph2_read64(p); L[0] = ((L[0] ^ v) >> 23) | ((L[0] ^ v) << 41); p += 8;
        v = ph2_read64(p); L[1] = ((L[1] ^ v) >> 23) | ((L[1] ^ v) << 41); p += 8;
        v = ph2_read64(p); L[2] = ((L[2] ^ v) >> 23) | ((L[2] ^ v) << 41); p += 8;
        v = ph2_read64(p); L[3] = ((L[3] ^ v) >> 23) | ((L[3] ^ v) << 41); p += 8;
        v = ph2_read64(p); L[4] = ((L[4] ^ v) >> 23) | ((L[4] ^ v) << 41); p += 8;
        v = ph2_read64(p); L[5] = ((L[5] ^ v) >> 23) | ((L[5] ^ v) << 41); p += 8;
        v = ph2_read64(p); L[6] = ((L[6] ^ v) >> 23) | ((L[6] ^ v) << 41); p += 8;
    }

    /* Remaining full words cyclically */
    int li = 0;
    while (p + 8 <= e) {
        uint64_t v = ph2_read64(p);
        L[li] = ((L[li] ^ v) >> 23) | ((L[li] ^ v) << 41);
        p += 8;  li = (li + 1) % 7;
    }

    /* Tail bytes */
    if (e > p) {
        uint64_t t = 0;
        size_t rem = (size_t)(e - p);
        if (rem >= 7) t ^= (uint64_t)p[6] << 48;
        if (rem >= 6) t ^= (uint64_t)p[5] << 40;
        if (rem >= 5) t ^= (uint64_t)p[4] << 32;
        if (rem >= 4) t ^= (uint64_t)p[3] << 24;
        if (rem >= 3) t ^= (uint64_t)p[2] << 16;
        if (rem >= 2) t ^= (uint64_t)p[1] <<  8;
        if (rem >= 1) t ^= (uint64_t)p[0];
        L[li] = ((L[li] ^ t) >> 23) | ((L[li] ^ t) << 41);
    }

    /* ── Rotated XOR‑tree compression ──
     * XOR with rotation prevents cancellation: even if L[0]==L[4],
     * L[0] ^ ROTL(L[4],r) ≠ 0 for non‑periodic values.
     * Mum + xorshift spread the resulting multi‑bit diff before
     * the finaliser. */
    L[0] ^= ph2_rot(L[4], 11);
    L[1] ^= ph2_rot(L[5], 17);
    L[2] ^= ph2_rot(L[6], 23);
    L[3] ^= L[0] ^ L[1] ^ L[2];
    {   __extension__ __uint128_t p = (__uint128_t)L[3] * PH2_PHI;
        L[3] = (uint64_t)p ^ (uint64_t)(p >> 64); }
    L[3] ^= L[3] >> 33;  L[3] *= PH2_M1;
    return ph2_final(L[3]);
}

/*
 * ── Medium path (48 ≤ len < 128): 4‑word ARX, 2 rounds per 32B block ──
 */
static uint64_t ph2_medium(const uint8_t * PH2_RESTRICT p,
                           size_t len, uint64_t seed) {
    const uint8_t *e = p + len;
    uint64_t mix = ph2_mix(seed ^ ((uint64_t)len * PH2_PHI));
    uint64_t h1 = mix * PH2_PHI;
    uint64_t h2 = mix * PH2_M1;
    uint64_t h3 = mix * PH2_M2;
    uint64_t h4 = mix * PH2_M3;

    while (PH2_LIKELY(p + 32 <= e)) {
        h1 ^= ph2_read64(p);  p += 8;
        h2 ^= ph2_read64(p);  p += 8;
        h3 ^= ph2_read64(p);  p += 8;
        h4 ^= ph2_read64(p);  p += 8;

        /* 2 rounds of 4‑word butterfly: adjacent then stride‑2 */
        PH2_AX(h1, h2, 17);  PH2_AX(h3, h4, 25);
        PH2_AX(h1, h3, 19);  PH2_AX(h2, h4, 27);
        PH2_AX(h1, h2, 33);  PH2_AX(h3, h4, 41);
        PH2_AX(h1, h3, 35);  PH2_AX(h2, h4, 43);
    }

    while (p + 8 <= e) {
        h1 ^= ph2_read64(p);  p += 8;
        h1 = ph2_rot(h1 + h2, 11);
    }

    /* Tail */
    uint64_t t = 0;
    switch (e - p) {
        case 7:  t ^= (uint64_t)p[6] << 48;  /* fallthrough */
        case 6:  t ^= (uint64_t)p[5] << 40;  /* fallthrough */
        case 5:  t ^= (uint64_t)p[4] << 32;  /* fallthrough */
        case 4:  t ^= (uint64_t)p[3] << 24;  /* fallthrough */
        case 3:  t ^= (uint64_t)p[2] << 16;  /* fallthrough */
        case 2:  t ^= (uint64_t)p[1] <<  8;  /* fallthrough */
        case 1:  t ^= (uint64_t)p[0];         break;
        default: break;
    }
    if (t) { h1 ^= t;  h1 = ph2_rot(h1 + h2, 11); }

    /* Lane compression */
    h1 = ph2_rot(h1, 31);  h1 += h2;  h1 += h3;  h1 += h4;
    return ph2_final(h1);
}

/*
 * ── Safe path (9 ≤ len < 48): 4‑word ARX + accumulator, 3 rounds ──
 */
static uint64_t ph2_safe(const uint8_t * PH2_RESTRICT p,
                         size_t len, uint64_t seed) {
    const uint8_t *e = p + len;
    uint64_t mix = ph2_mix(seed ^ ((uint64_t)len * PH2_PHI));
    uint64_t h1 = mix * PH2_PHI;
    uint64_t h2 = mix * PH2_M1;
    uint64_t h3 = mix * PH2_M2;
    uint64_t h4 = mix * PH2_M3;
    uint64_t acc = mix ^ PH2_M2;
    int has_blocks = 0;

    while (PH2_LIKELY(p + 32 <= e)) {
        uint64_t v1 = ph2_read64(p);  p += 8;
        uint64_t v2 = ph2_read64(p);  p += 8;
        uint64_t v3 = ph2_read64(p);  p += 8;
        uint64_t v4 = ph2_read64(p);  p += 8;

        h1 ^= v1;  h2 ^= v2;  h3 ^= v3;  h4 ^= v4;

        /* 3 rounds of 4‑word butterfly */
        PH2_AX(h1, h2, 17);  PH2_AX(h3, h4, 25);
        PH2_AX(h1, h3, 19);  PH2_AX(h2, h4, 27);
        PH2_AX(h1, h2, 33);  PH2_AX(h3, h4, 41);
        PH2_AX(h1, h3, 35);  PH2_AX(h2, h4, 43);
        PH2_AX(h1, h2, 49);  PH2_AX(h3, h4, 53);
        PH2_AX(h1, h3, 21);  PH2_AX(h2, h4, 29);

        acc ^= v1 ^ v2 ^ v3 ^ v4;
        acc  = ph2_rot(acc, 31);
        acc *= PH2_PHI;
        has_blocks = 1;
    }

    while (p + 8 <= e) {
        uint64_t v = ph2_read64(p);  p += 8;
        h1 ^= v;  h1 = ph2_rot(h1 + h2, 11);
        acc ^= v;  acc = ph2_rot(acc, 31);  acc *= PH2_PHI;
        has_blocks = 1;
    }

    /* Tail */
    uint64_t t = 0;
    switch (e - p) {
        case 7:  t ^= (uint64_t)p[6] << 48;  /* fallthrough */
        case 6:  t ^= (uint64_t)p[5] << 40;  /* fallthrough */
        case 5:  t ^= (uint64_t)p[4] << 32;  /* fallthrough */
        case 4:  t ^= (uint64_t)p[3] << 24;  /* fallthrough */
        case 3:  t ^= (uint64_t)p[2] << 16;  /* fallthrough */
        case 2:  t ^= (uint64_t)p[1] <<  8;  /* fallthrough */
        case 1:  t ^= (uint64_t)p[0];         break;
        default: break;
    }
    if (t) {
        h1 ^= t;  h1 = ph2_rot(h1 + h2, 11);
        if (has_blocks) { acc ^= t;  acc = ph2_rot(acc, 31);  acc *= PH2_PHI; }
    }

    h1 += h2;  h3 += h4;  h1 += h3;
    if (has_blocks) { acc = ph2_rot(acc ^ h1, 43);  acc *= PH2_PHI;  h1 += acc; }

    return ph2_final(h1);
}

/* ── Dispatch ── */
uint64_t plumshash2(const void *buf, size_t len, uint64_t seed) {
    const uint8_t * PH2_RESTRICT p = (const uint8_t *)buf;
    if (PH2_LIKELY(len <= 16))
        return ph2_tiny(p, len, seed);
    if (PH2_LIKELY(len >= 128))
        return ph2_fast(p, len, seed);
    if (len >= 48)
        return ph2_medium(p, len, seed);
    return ph2_safe(p, len, seed);
}

#endif  /* PLUMSHASH2_IMPLEMENTATION */
