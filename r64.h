/* R64-SCAN — parameterized search framework for BITSCAN
 * SPDX-License-Identifier: MPL-2.0
 *
 * Compile with:
 *   gcc -DR64_LANES=7 -DR64_MIXER=ROR -DR64_ROTK=23 \
 *       -DR64_TREE=PAIRWISE -O3 -o r64scan r64scan.c
 *
 * Then run through SMHasher:
 *   ./SMHasher --test= Avalanche,Distribution,Seed r64scan
 */

#ifndef R64_H
#define R64_H
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#ifdef __cplusplus
extern "C" {
#endif
uint64_t r64hash(const void *buf, size_t len, uint64_t seed);
#ifdef __cplusplus
}
#endif
#endif /* R64_H */

#ifdef R64_IMPLEMENTATION

/* ── tunable parameters ── */
#ifndef R64_LANES
#define R64_LANES 7
#endif

#ifndef R64_ROTK
#define R64_ROTK 23
#endif

/* Mixer selection */
#define R64_MIXER_ROR    1
#define R64_MIXER_RBIT   2
#define R64_MIXER_REV    3
#define R64_MIXER_XORSHR 4
#define R64_MIXER_ADDLSL 5

#ifndef R64_MIXER
#define R64_MIXER R64_MIXER_ROR
#endif

/* Compression tree selection */
#define R64_TREE_PAIRWISE  1
#define R64_TREE_ADJACENT  2
#define R64_TREE_MIRROR    3
#define R64_TREE_CASCADE   4
#define R64_TREE_BALANCED  5

#ifndef R64_TREE
#define R64_TREE R64_TREE_PAIRWISE
#endif

/* Finalizer constants (tunable) */
#ifndef R64_FIN_C1
#define R64_FIN_C1 0xC2B2AE3D27D4EB4FULL
#endif
#ifndef R64_FIN_C2
#define R64_FIN_C2 0x9E3779B97F4A7C15ULL
#endif

/* ── helpers ── */
static inline uint64_t r64_rotr(uint64_t x, int k) {
    return (x >> k) | (x << (64 - k));
}

static inline uint64_t r64_mix(uint64_t h) {
#if R64_MIXER == R64_MIXER_ROR
    return r64_rotr(h, R64_ROTK);
#elif R64_MIXER == R64_MIXER_RBIT
    uint64_t r; __asm__("rbit %0, %1" : "=r"(r) : "r"(h)); return r;
#elif R64_MIXER == R64_MIXER_REV
    uint64_t r; __asm__("rev %0, %1" : "=r"(r) : "r"(h)); return r;
#elif R64_MIXER == R64_MIXER_XORSHR
    return h ^ (h >> 27);
#elif R64_MIXER == R64_MIXER_ADDLSL
    return h + (h << 3);
#else
#error "Unknown R64_MIXER"
#endif
}

/* ── init constants ── */
static const uint64_t r64_init[15] = {
    0x9E3779B97F4A7C15ULL, 0xBF58476D1CE4E5B9ULL, 0x94D049BB133111EBULL,
    0xC2B2AE3D27D4EB4FULL, 0x85EBCA77C2B2AE63ULL,
    0x27D4EB2F165667C5ULL, 0x165667B19E3779F9ULL,
    0x9E3779B185EBCA77ULL, 0xBF58476D27D4EB2FULL,
    0x94D049BB165667B1ULL, 0xC2B2AE3D9E3779B9ULL,
    0x85EBCA77BF58476DULL, 0x27D4EB2F94D049BBULL,
    0x165667B1C2B2AE3DULL, 0x9E3779B927D4EB2FULL,
};

/* ── compression tree ── */
#if R64_TREE == R64_TREE_PAIRWISE
#define R64_COMPRESS(L) do { L[0]^=L[4];L[1]^=L[5];L[2]^=L[6];L[3]^=L[0]^L[1]^L[2]; } while(0)
#define R64_OUTLANE 3
#elif R64_TREE == R64_TREE_CASCADE
#define R64_COMPRESS(L) do { L[0]^=L[1];L[0]^=L[2];L[0]^=L[3];L[0]^=L[4];L[0]^=L[5];L[0]^=L[6]; } while(0)
#define R64_OUTLANE 0
#elif R64_TREE == R64_TREE_BALANCED
#define R64_COMPRESS(L) do { L[0]^=L[1];L[2]^=L[3];L[4]^=L[5];L[0]^=L[2];L[4]^=L[6];L[0]^=L[4]; } while(0)
#define R64_OUTLANE 0
#elif R64_TREE == R64_TREE_MIRROR
#define R64_COMPRESS(L) do { L[0]^=L[6];L[1]^=L[5];L[2]^=L[4];L[3]^=L[0]^L[1]^L[2]; } while(0)
#define R64_OUTLANE 3
#elif R64_TREE == R64_TREE_ADJACENT
#define R64_COMPRESS(L) do { L[0]^=L[1];L[2]^=L[3];L[4]^=L[5];L[6]^=L[0]^L[2]^L[4]; } while(0)
#define R64_OUTLANE 6
#else
#error "Unknown R64_TREE"
#endif

uint64_t r64hash(const void *buf, size_t len, uint64_t seed) {
    const uint8_t *p = (const uint8_t *)buf;
    const uint8_t *e = p + len;
    uint64_t L[R64_LANES];
    for (int i = 0; i < R64_LANES; i++) L[i] = r64_mix(seed ^ r64_init[i]);
    L[0] ^= len;
    while (p + (R64_LANES * 8) <= e) {
        for (int i = 0; i < R64_LANES; i++) {
            uint64_t v; memcpy(&v, p, 8); p += 8; L[i] = r64_mix(L[i] ^ v);
        }
    }
    int li = 0;
    while (p + 8 <= e) {
        uint64_t v; memcpy(&v, p, 8); p += 8;
        L[li] = r64_mix(L[li] ^ v); li = (li + 1) % R64_LANES;
    }
    if (e > p) {
        uint64_t t = 0; size_t rem = (size_t)(e - p);
        if(rem>=7)t^=(uint64_t)p[6]<<48; if(rem>=6)t^=(uint64_t)p[5]<<40;
        if(rem>=5)t^=(uint64_t)p[4]<<32; if(rem>=4)t^=(uint64_t)p[3]<<24;
        if(rem>=3)t^=(uint64_t)p[2]<<16; if(rem>=2)t^=(uint64_t)p[1]<<8;
        if(rem>=1)t^=(uint64_t)p[0];
        L[li] = r64_mix(L[li] ^ t);
    }
    R64_COMPRESS(L);
    uint64_t h = L[R64_OUTLANE];
    h ^= h >> 33; h *= R64_FIN_C1;
    h ^= h >> 29; h *= R64_FIN_C2;
    h ^= h >> 32;
    return h;
}
#endif
