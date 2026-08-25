/*
 * plumshash_hybrid.h — plumshash hybrid: 5-region, empirically selected
 * =========================================================================
 * Design (benchmarked on AArch64 Snapdragon, clang -O3 -march=native,
 * latency + throughput harnesses, min-of-3, order-randomized against DVFS):
 *
 *   len <= 3    xxh3-style tiny     single-width mul, no seed premix (2 muls)
 *   len <= 16   rapidhash-style mum 3-mul mum chain. Selected for 4-16B:
 *                                   beats xxh3's rrmxmx at 4-8B, and the
 *                                   mum fold passes the strict SMHasher BIC
 *                                   (the xxh3-style 9-16B fold fails BIC on
 *                                   11-byte keys regardless of constants;
 *                                   real xxh3 fails it on this build too)
 *   len <= 112  xxh3-style mid      independent mix16B pairs (ILP), wins
 *                                   20-45% latency over serial mum chains
 *   len <= 288  mum 7-lane          rapidhash >112 structure (112B/224B
 *                                   unrolled, 7 parallel lanes) — wins the
 *                                   113-288B window on both harnesses
 *                                   (113-128 joins via the 112B parallel
 *                                   block + tail, beating the mid path's
 *                                   serial add chain; 129-224 replaced the
 *                                   xxh3-style large walk: same latency
 *                                   within noise, 15-19% better throughput)
 *   len >  288  rotr23 7-lane       plums_fast port (168B unroll, ldp pairs,
 *                                   rotated compression) + one mum per lane
 *                                   (breaks the linear data path that made
 *                                   the pure-XOR tree collapse low-weight
 *                                   keys on SMHasher Combination tests) +
 *                                   length whitening
 *
 * Region boundaries were selected from measured crossover points; each
 * region is the fastest of the candidates at its sizes on this device.
 *
 * Provenance: the <=112B paths mirror the structure of xxh3's short-key
 * functions (BSD-2-Clause, Yann Collet) and rapidhash's tiny mum path
 * (MIT, Nicoshev) with plumshash-family constants; the 113-288B path
 * mirrors rapidhash's >112 mum structure (MIT, Nicoshev); the >288B path
 * is the plumshash fast path (MPL-2.0). All constants are
 * plumshash-family. Note the xxh3-derived mid path shares xxh3's
 * documented seed-dependent multicollision property (17-112B,
 * multiplication by zero).
 *
 * STATUS: benchmarked on one SoC, one compiler, one input pattern. Verify
 * on your target hardware before trusting it in production.
 *
 * SPDX-License-Identifier: MPL-2.0
 */
#ifndef PLUMSHASH_HYBRID_H
#define PLUMSHASH_HYBRID_H
#include <stdint.h>
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif
uint64_t plumshash_hybrid(const void *buf, size_t len, uint64_t seed);
#ifdef __cplusplus
}
#endif
#endif /* PLUMSHASH_HYBRID_H */

#ifdef PLUMSHASH_HYBRID_IMPLEMENTATION
#include <string.h>

#if defined(__GNUC__) || defined(__clang__)
  #define PH_INLINE inline __attribute__((always_inline))
  #define PH_LIKELY(x)   __builtin_expect(!!(x), 1)
  #define PH_RESTRICT __restrict__
#else
  #define PH_INLINE inline
  #define PH_LIKELY(x)   (x)
  #define PH_RESTRICT
#endif

#define PH_PHI 0x9E3779B97F4A7C15ULL
#define PH_M1  0x85EBCA77C2B2AE3DULL
#define PH_M2  0xBF58476D1CE4E5B9ULL
#define PH_M3  0x94D049BB133111EBULL
#define PH_RMX 0x9FB21C651E98DF25ULL   /* rrmxmx multiplier */
#define PH_AVL 0x165667919E3779F9ULL   /* avalanche multiplier */

/* 136-byte secret for the xxh3-style paths (plumshash family, all odd) */
static const uint64_t ph_sec[17] = {
    PH_PHI, PH_M1, PH_M2, PH_M3, 0x2545F4914F6CDD1DULL, PH_RMX,
    0xC2B2AE3D27D4EB4FULL, 0xD6E8FEB86659FD93ULL, 0x165667B19E3779F9ULL,
    0x27D4EB2F165667C5ULL, 0x85EBCA77C2B2AE63ULL, PH_AVL,
    0xA0761D6478BD642FULL, 0xE7037ED1A0B428DBULL, 0x90ED1765281C388CULL,
    0x2D358DCCAA6C78A5ULL, 0x8BB84B93962EACC9ULL,
};
/* precomputed secret XOR pairs for the tiny 0-3B path (BIC-verified) */
#define PH_SEC_13   0xBDF8D228ULL                    /* (u32)PHI ^ (u32)M1 */
#define PH_SEC_0    (ph_sec[7] ^ ph_sec[8])          /* secret[56]^secret[64] */

/* 8 mum-path domain constants (plumshash family, all odd) */
static const uint64_t ph_S[8] = {
    PH_PHI, PH_M1, PH_M2, PH_M3, 0x2545F4914F6CDD1DULL, PH_RMX,
    0xC2B2AE3D27D4EB4FULL, 0xD6E8FEB86659FD93ULL,
};
/* 7 rotr23-lane init constants (plumshash fast-path) */
static const uint64_t ph_rot_init[7] = {
    0x9E3779B97F4A7C15ULL, 0xBF58476D1CE4E5B9ULL, 0x94D049BB133111EBULL,
    0xC2B2AE3D27D4EB4FULL, 0x85EBCA77C2B2AE63ULL,
    0x27D4EB2F165667C5ULL, 0x165667B19E3779F9ULL,
};

static PH_INLINE uint64_t ph_read64(const uint8_t *p) { uint64_t v; memcpy(&v,p,8); return v; }
static PH_INLINE uint32_t ph_read32(const uint8_t *p) { uint32_t v; memcpy(&v,p,4); return v; }
static PH_INLINE uint64_t ph_rot(uint64_t x, int k) { return (x<<k)|(x>>(64-k)); }
static PH_INLINE uint64_t ph_rotr23(uint64_t x) { return (x >> 23) | (x << 41); }
static PH_INLINE uint64_t ph_mum(uint64_t a, uint64_t b) {
    __uint128_t r = (__uint128_t)a * b;
    return (uint64_t)r ^ (uint64_t)(r >> 64);
}
static PH_INLINE void ph_mum2(uint64_t a, uint64_t b, uint64_t *lo, uint64_t *hi) {
    __uint128_t r = (__uint128_t)a * b;
    *lo = (uint64_t)r; *hi = (uint64_t)(r >> 64);
}
static PH_INLINE uint64_t ph_mix_seed(uint64_t seed) {
    seed ^= 0xD6E8FEB86659FD93ULL;   /* domain constant: seed=0 must not stay 0 */
    seed ^= seed >> 33; seed *= PH_M1; seed ^= seed >> 29;
    return seed;
}
static PH_INLINE uint64_t ph_final_plums(uint64_t h) {
    h ^= h >> 29;  h *= PH_M1;
    h ^= h >> 31;  h *= PH_M2;
    h ^= h >> 37;  h *= PH_PHI;
    h ^= h >> 41;
    return h;
}

/* ── xxh3-style primitives ── */
static PH_INLINE uint64_t ph_aval(uint64_t h) {
    h ^= h >> 37; h *= PH_AVL; h ^= h >> 32; return h;
}
static PH_INLINE uint64_t ph_m128f(uint64_t a, uint64_t b) {
    __uint128_t r = (__uint128_t)a * b;
    return (uint64_t)r ^ (uint64_t)(r >> 64);
}
static PH_INLINE uint64_t ph_mix16B(const uint8_t *in, uint64_t s0, uint64_t s1,
                                    uint64_t seed) {
    return ph_m128f(ph_read64(in) ^ (s0 + seed), ph_read64(in + 8) ^ (s1 - seed));
}

/* ── 0-3 bytes: single-width mul, no seed premix (2-mul chain) ── */
static PH_INLINE uint64_t ph_x13(const uint8_t *p, size_t len, uint64_t seed) {
    if (len) {
        uint32_t c1 = p[0], c2 = p[len >> 1], c3 = p[len - 1];
        uint32_t combined = (c1 << 16) | (c2 << 24) | c3 | ((uint32_t)len << 8);
        return ph_aval(((uint64_t)combined ^ (PH_SEC_13 + seed)) * PH_PHI);
    }
    return ph_aval((PH_PHI + seed) ^ PH_SEC_0);
}

/* ── 17-128 bytes: independent mix16B pairs (ILP) ── */
static uint64_t ph_mid(const uint8_t * PH_RESTRICT p, size_t len, uint64_t seed) {
    uint64_t acc = (uint64_t)len * PH_PHI;
    if (len > 32) {
        if (len > 64) {
            if (len > 96) {
                acc += ph_mix16B(p + 48, ph_sec[12], ph_sec[13], seed);
                acc += ph_mix16B(p + len - 64, ph_sec[14], ph_sec[15], seed);
            }
            acc += ph_mix16B(p + 32, ph_sec[8], ph_sec[9], seed);
            acc += ph_mix16B(p + len - 48, ph_sec[10], ph_sec[11], seed);
        }
        acc += ph_mix16B(p + 16, ph_sec[4], ph_sec[5], seed);
        acc += ph_mix16B(p + len - 32, ph_sec[6], ph_sec[7], seed);
    }
    acc += ph_mix16B(p, ph_sec[0], ph_sec[1], seed);
    acc += ph_mix16B(p + len - 16, ph_sec[2], ph_sec[3], seed);
    return ph_aval(acc);
}

/* ── Mum path (rapidhash structure, plumshash constants): used for the
 * 4-16B tiny case (beats xxh3's rrmxmx there) and the 129-288B window
 * (7-lane 112B/224B unrolled blocks; wins that window on both harnesses).
 * Length is folded into the final multiply, so length separation cannot
 * be cancelled by a linear tree. ── */
static uint64_t ph_mum_path(const uint8_t * PH_RESTRICT p, size_t len, uint64_t seed) {
    size_t i = len;
    seed ^= ph_mum(seed ^ ph_S[2], ph_S[1]);
    uint64_t a = 0, b = 0;

    if (PH_LIKELY(len <= 16)) {
        if (len >= 4) {
            seed ^= len;
            if (len > 8) { a = ph_read64(p); b = ph_read64(p + len - 8); }
            else { a = ph_read32(p); b = ph_read32(p + len - 4); }
        } else if (len > 0) {
            a = ((uint64_t)p[0] << 45) | p[len - 1];
            b = p[len >> 1];
        }
    } else {
        if (i > 112) {
            uint64_t see1 = seed, see2 = seed, see3 = seed;
            uint64_t see4 = seed, see5 = seed, see6 = seed;
            while (i > 224) {
                seed = ph_mum(ph_read64(p)      ^ ph_S[0], ph_read64(p +  8) ^ seed);
                see1 = ph_mum(ph_read64(p + 16) ^ ph_S[1], ph_read64(p + 24) ^ see1);
                see2 = ph_mum(ph_read64(p + 32) ^ ph_S[2], ph_read64(p + 40) ^ see2);
                see3 = ph_mum(ph_read64(p + 48) ^ ph_S[3], ph_read64(p + 56) ^ see3);
                see4 = ph_mum(ph_read64(p + 64) ^ ph_S[4], ph_read64(p + 72) ^ see4);
                see5 = ph_mum(ph_read64(p + 80) ^ ph_S[5], ph_read64(p + 88) ^ see5);
                see6 = ph_mum(ph_read64(p + 96) ^ ph_S[6], ph_read64(p +104) ^ see6);
                seed = ph_mum(ph_read64(p +112) ^ ph_S[0], ph_read64(p +120) ^ seed);
                see1 = ph_mum(ph_read64(p +128) ^ ph_S[1], ph_read64(p +136) ^ see1);
                see2 = ph_mum(ph_read64(p +144) ^ ph_S[2], ph_read64(p +152) ^ see2);
                see3 = ph_mum(ph_read64(p +160) ^ ph_S[3], ph_read64(p +168) ^ see3);
                see4 = ph_mum(ph_read64(p +176) ^ ph_S[4], ph_read64(p +184) ^ see4);
                see5 = ph_mum(ph_read64(p +192) ^ ph_S[5], ph_read64(p +200) ^ see5);
                see6 = ph_mum(ph_read64(p +208) ^ ph_S[6], ph_read64(p +216) ^ see6);
                p += 224; i -= 224;
            }
            if (i > 112) {
                seed = ph_mum(ph_read64(p)      ^ ph_S[0], ph_read64(p +  8) ^ seed);
                see1 = ph_mum(ph_read64(p + 16) ^ ph_S[1], ph_read64(p + 24) ^ see1);
                see2 = ph_mum(ph_read64(p + 32) ^ ph_S[2], ph_read64(p + 40) ^ see2);
                see3 = ph_mum(ph_read64(p + 48) ^ ph_S[3], ph_read64(p + 56) ^ see3);
                see4 = ph_mum(ph_read64(p + 64) ^ ph_S[4], ph_read64(p + 72) ^ see4);
                see5 = ph_mum(ph_read64(p + 80) ^ ph_S[5], ph_read64(p + 88) ^ see5);
                see6 = ph_mum(ph_read64(p + 96) ^ ph_S[6], ph_read64(p +104) ^ see6);
                p += 112; i -= 112;
            }
            seed ^= see1; see2 ^= see3; see4 ^= see5;
            seed ^= see6; see2 ^= see4; seed ^= see2;
        }
        if (i > 16) {
            seed = ph_mum(ph_read64(p)      ^ ph_S[2], ph_read64(p +  8) ^ seed);
            if (i > 32) {
                seed = ph_mum(ph_read64(p + 16) ^ ph_S[2], ph_read64(p + 24) ^ seed);
                if (i > 48) {
                    seed = ph_mum(ph_read64(p + 32) ^ ph_S[1], ph_read64(p + 40) ^ seed);
                    if (i > 64) {
                        seed = ph_mum(ph_read64(p + 48) ^ ph_S[1], ph_read64(p + 56) ^ seed);
                        if (i > 80) {
                            seed = ph_mum(ph_read64(p + 64) ^ ph_S[2], ph_read64(p + 72) ^ seed);
                            if (i > 96) {
                                seed = ph_mum(ph_read64(p + 80) ^ ph_S[1], ph_read64(p + 88) ^ seed);
                            }
                        }
                    }
                }
            }
        }
        a = ph_read64(p + i - 16) ^ i;
        b = ph_read64(p + i - 8);
    }

    a ^= ph_S[1];
    b ^= seed;
    ph_mum2(a, b, &a, &b);
    return ph_mum(a ^ ph_S[7], b ^ ph_S[1] ^ i);
}

/* ── rotr23 7-lane bulk (len > 256): plums_fast port ──
 * 168B unrolled blocks (21 loads hoisted, ldp-pair friendly), 56B blocks,
 * if-chain remainder words, memcpy tail, rotated balanced compression
 * tree, then 4-round finalizer + length/seed whitening re-injected after
 * compression (prevents linear-tree length-cancellation). Cheap lane init. */
static uint64_t ph_rot_bulk(const uint8_t * PH_RESTRICT p, size_t len, uint64_t seed) {
    const uint8_t *e = p + len;
    uint64_t mixed = ph_mix_seed(seed);
    uint64_t L[7];
    for (int i = 0; i < 7; i++)
        L[i] = ph_rotr23(mixed ^ ph_rot_init[i]);
    L[0] ^= len;

    while (PH_LIKELY(p + 168 <= e)) {
        uint64_t v0  = ph_read64(p +   0), v1  = ph_read64(p +   8),
                 v2  = ph_read64(p +  16), v3  = ph_read64(p +  24),
                 v4  = ph_read64(p +  32), v5  = ph_read64(p +  40),
                 v6  = ph_read64(p +  48), v7  = ph_read64(p +  56),
                 v8  = ph_read64(p +  64), v9  = ph_read64(p +  72),
                 v10 = ph_read64(p +  80), v11 = ph_read64(p +  88),
                 v12 = ph_read64(p +  96), v13 = ph_read64(p + 104),
                 v14 = ph_read64(p + 112), v15 = ph_read64(p + 120),
                 v16 = ph_read64(p + 128), v17 = ph_read64(p + 136),
                 v18 = ph_read64(p + 144), v19 = ph_read64(p + 152),
                 v20 = ph_read64(p + 160);
        p += 168;
        L[0] = ph_rotr23(L[0] ^ v0);   L[1] = ph_rotr23(L[1] ^ v1);
        L[2] = ph_rotr23(L[2] ^ v2);   L[3] = ph_rotr23(L[3] ^ v3);
        L[4] = ph_rotr23(L[4] ^ v4);   L[5] = ph_rotr23(L[5] ^ v5);
        L[6] = ph_rotr23(L[6] ^ v6);
        L[0] = ph_rotr23(L[0] ^ v7);   L[1] = ph_rotr23(L[1] ^ v8);
        L[2] = ph_rotr23(L[2] ^ v9);   L[3] = ph_rotr23(L[3] ^ v10);
        L[4] = ph_rotr23(L[4] ^ v11);  L[5] = ph_rotr23(L[5] ^ v12);
        L[6] = ph_rotr23(L[6] ^ v13);
        L[0] = ph_rotr23(L[0] ^ v14);  L[1] = ph_rotr23(L[1] ^ v15);
        L[2] = ph_rotr23(L[2] ^ v16);  L[3] = ph_rotr23(L[3] ^ v17);
        L[4] = ph_rotr23(L[4] ^ v18);  L[5] = ph_rotr23(L[5] ^ v19);
        L[6] = ph_rotr23(L[6] ^ v20);
    }
    while (PH_LIKELY(p + 56 <= e)) {
        uint64_t v0 = ph_read64(p +  0), v1 = ph_read64(p +  8),
                 v2 = ph_read64(p + 16), v3 = ph_read64(p + 24),
                 v4 = ph_read64(p + 32), v5 = ph_read64(p + 40),
                 v6 = ph_read64(p + 48);
        p += 56;
        L[0] = ph_rotr23(L[0] ^ v0);  L[1] = ph_rotr23(L[1] ^ v1);
        L[2] = ph_rotr23(L[2] ^ v2);  L[3] = ph_rotr23(L[3] ^ v3);
        L[4] = ph_rotr23(L[4] ^ v4);  L[5] = ph_rotr23(L[5] ^ v5);
        L[6] = ph_rotr23(L[6] ^ v6);
    }
    {
        int rn = (int)((e - p) >> 3);
        if (rn > 0) { L[0] = ph_rotr23(L[0] ^ ph_read64(p)); p += 8; }
        if (rn > 1) { L[1] = ph_rotr23(L[1] ^ ph_read64(p)); p += 8; }
        if (rn > 2) { L[2] = ph_rotr23(L[2] ^ ph_read64(p)); p += 8; }
        if (rn > 3) { L[3] = ph_rotr23(L[3] ^ ph_read64(p)); p += 8; }
        if (rn > 4) { L[4] = ph_rotr23(L[4] ^ ph_read64(p)); p += 8; }
        if (rn > 5) { L[5] = ph_rotr23(L[5] ^ ph_read64(p)); p += 8; }
        if (e > p) {
            uint64_t t = 0;
            memcpy(&t, p, (size_t)(e - p));
            L[rn] = ph_rotr23(L[rn] ^ t);
        }
    }
    /* Nonlinear lane folding: the rotr23 lane mixes are linear in the data,
     * and a pure XOR compression tree collapses low-weight keys (SMHasher
     * Combination 16/32-byte-block tests: 100K keys -> ~4K distinct hashes).
     * One mum per lane (parallel, off the critical path) breaks the
     * linearity before the compression tree. */
    for (int i = 0; i < 7; i++)
        L[i] = ph_mum(L[i], ph_rot_init[i] ^ mixed);
    L[0] ^= ph_rot(L[4], 11);
    L[1] ^= ph_rot(L[5], 17);
    L[2] ^= ph_rot(L[6], 23);
    L[0] ^= L[1];
    L[2] ^= L[3];
    L[0] ^= L[2];
    L[0] ^= ph_rot(L[0], 2);
    uint64_t h = ph_final_plums(L[0]);
    h ^= mixed ^ (len * PH_PHI);   /* length/seed whitening after compression */
    return h;
}

uint64_t plumshash_hybrid(const void *buf, size_t len, uint64_t seed) {
    const uint8_t * PH_RESTRICT p = (const uint8_t *)buf;
    if (PH_LIKELY(len <= 3))   return ph_x13(p, len, seed);
    if (len <= 16)             return ph_mum_path(p, len, seed);   /* 4-16: mum tiny */
    if (len <= 112)            return ph_mid(p, len, seed);
    if (len <= 288)            return ph_mum_path(p, len, seed);   /* 113-288: mum 7-lane */
    return ph_rot_bulk(p, len, seed);
}

#endif /* PLUMSHASH_HYBRID_IMPLEMENTATION */
