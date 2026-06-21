/*
 * bench_compare.c — side‑by‑side benchmark of 64‑bit hash functions
 *
 * Tests: PlumsHash, wyhash, xxHash64, FastHash64
 * Metrics: GB/s, B/s, ns/hash for keys 4B–4KB, avalanche, χ²
 *
 * Compile:  gcc -O3 -Wall -Wextra -o bench_compare bench_compare.c
 * Run:      ./bench_compare
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#include <math.h>

/* ═══════════════════════════════════════════════════════════════════════
 * Hash implementations (all single‑header, inline)
 * ═══════════════════════════════════════════════════════════════════════ */

/* ── PlumsHash ── */
#define PLUMSHASH_IMPLEMENTATION
#include "plumshash.h"

/* ── wyhash (full implementation from wyhash.h) ── */
#include "wyhash.h"
/* wyhash takes 4 args — wrap with default secret */
static inline uint64_t wyhash_for_bench(const void *key, size_t len, uint64_t seed) {
    return wyhash(key, len, seed, _wyp);
}

/* ── rapidhash V3 (fast wyhash derivative) ── */
#define RAPIDHASH_IMPL
#include "rapidhash.h"
static inline uint64_t rapidhash_for_bench(const void *key, size_t len, uint64_t seed) {
    return rapidhash_withSeed(key, len, seed);
}

/* ── R64 (PlumsHash v2 candidate: 7-lane rotr23) ── */
#define R64_IMPLEMENTATION
#include "r64.h"
static inline uint64_t r64_for_bench(const void *key, size_t len, uint64_t seed) {
    return r64hash(key, len, seed);
}

/* ── xxHash64 (canonical oneshot, Yann Collet) ── */
#define XXH_PRIME64_1 0x9E3779B185EBCA87ULL
#define XXH_PRIME64_2 0xC2B2AE3D27D4EB4FULL
#define XXH_PRIME64_3 0x165667B19E3779F9ULL
#define XXH_PRIME64_4 0x85EBCA77C2B2AE63ULL
#define XXH_PRIME64_5 0x27D4EB2F165667C5ULL

static inline uint64_t XXH_rotl64(uint64_t x, int r) {
    return (x << r) | (x >> (64 - r));
}
static inline uint64_t XXH_read64(const void *p) {
    uint64_t v; memcpy(&v, p, 8); return v;
}
static inline uint32_t XXH_read32(const void *p) {
    uint32_t v; memcpy(&v, p, 4); return v;
}
static inline uint64_t XXH64_round(uint64_t acc, uint64_t input) {
    acc += input * XXH_PRIME64_2;
    acc  = XXH_rotl64(acc, 31);
    acc *= XXH_PRIME64_1;
    return acc;
}
static inline uint64_t XXH64_mergeRound(uint64_t acc, uint64_t val) {
    val  = XXH64_round(0, val);
    acc ^= val;
    acc  = acc * XXH_PRIME64_1 + XXH_PRIME64_4;
    return acc;
}
static uint64_t xxhash64(const void *key, size_t len, uint64_t seed) {
    const uint8_t *p = (const uint8_t *)key;
    const uint8_t *const end = p + len;
    uint64_t h64;

    if (len >= 32) {
        const uint8_t *const limit = end - 32;
        uint64_t v1 = seed + XXH_PRIME64_1 + XXH_PRIME64_2;
        uint64_t v2 = seed + XXH_PRIME64_2;
        uint64_t v3 = seed + 0;
        uint64_t v4 = seed - XXH_PRIME64_1;
        do {
            v1 = XXH64_round(v1, XXH_read64(p)); p += 8;
            v2 = XXH64_round(v2, XXH_read64(p)); p += 8;
            v3 = XXH64_round(v3, XXH_read64(p)); p += 8;
            v4 = XXH64_round(v4, XXH_read64(p)); p += 8;
        } while (p <= limit);
        h64 = XXH_rotl64(v1, 1) + XXH_rotl64(v2, 7)
            + XXH_rotl64(v3, 12) + XXH_rotl64(v4, 18);
        h64 = XXH64_mergeRound(h64, v1);
        h64 = XXH64_mergeRound(h64, v2);
        h64 = XXH64_mergeRound(h64, v3);
        h64 = XXH64_mergeRound(h64, v4);
    } else {
        h64 = seed + XXH_PRIME64_5;
    }
    h64 += len;

    while (p + 8 <= end) {
        uint64_t k1 = XXH64_round(0, XXH_read64(p));
        h64 ^= k1;
        h64  = XXH_rotl64(h64, 27) * XXH_PRIME64_1 + XXH_PRIME64_4;
        p += 8;
    }
    if (p + 4 <= end) {
        h64 ^= (uint64_t)XXH_read32(p) * XXH_PRIME64_1;
        h64  = XXH_rotl64(h64, 23) * XXH_PRIME64_2 + XXH_PRIME64_3;
        p += 4;
    }
    while (p < end) {
        h64 ^= (*p) * XXH_PRIME64_5;
        h64  = XXH_rotl64(h64, 11) * XXH_PRIME64_1;
        p++;
    }

    h64 ^= h64 >> 33;
    h64 *= XXH_PRIME64_2;
    h64 ^= h64 >> 29;
    h64 *= XXH_PRIME64_3;
    h64 ^= h64 >> 32;
    return h64;
}

/* ── FastHash64 (Zilong Tan, condensed) ── */
static inline uint64_t fh_mix(uint64_t h) {
    h ^= h >> 23;
    h *= 0x2127599bf4325c37ULL;
    h ^= h >> 47;
    return h;
}
static uint64_t fasthash64(const void *buf, size_t len, uint64_t seed) {
    const uint8_t *p = (const uint8_t *)buf;
    const uint8_t *const end = p + len;
    uint64_t h = seed ^ (len * 0x880355f21e6d1965ULL);
    uint64_t v;

    while (p + 8 <= end) {
        memcpy(&v, p, 8);
        h ^= fh_mix(v);
        h *= 0x880355f21e6d1965ULL;
        p += 8;
    }
    v = 0;
    switch (end - p) {
        case 7: v ^= (uint64_t)p[6] << 48; /* fallthrough */
        case 6: v ^= (uint64_t)p[5] << 40; /* fallthrough */
        case 5: v ^= (uint64_t)p[4] << 32; /* fallthrough */
        case 4: v ^= (uint64_t)p[3] << 24; /* fallthrough */
        case 3: v ^= (uint64_t)p[2] << 16; /* fallthrough */
        case 2: v ^= (uint64_t)p[1] <<  8; /* fallthrough */
        case 1: v ^= (uint64_t)p[0];
                h ^= fh_mix(v);
                h *= 0x880355f21e6d1965ULL;
                break;
    }
    return fh_mix(h);
}

/* ═══════════════════════════════════════════════════════════════════════
 * Benchmark harness
 * ═══════════════════════════════════════════════════════════════════════ */

typedef struct {
    const char *name;
    uint64_t (*hash)(const void*, size_t, uint64_t);
} hash_t;

static int popcount(uint64_t x) {
    x -= (x >> 1) & 0x5555555555555555ULL;
    x  = (x & 0x3333333333333333ULL) + ((x >> 2) & 0x3333333333333333ULL);
    x  = (x + (x >> 4)) & 0x0F0F0F0F0F0F0F0FULL;
    return (int)((x * 0x0101010101010101ULL) >> 56);
}

static double avalanche_worst(uint64_t (*hash)(const void*,size_t,uint64_t),
                              int keylen) {
    uint8_t *buf = (uint8_t*)calloc(1, keylen);
    for (int i = 0; i < keylen; i++) buf[i] = (uint8_t)(i * 0x9D + 0x37);
    double worst = 100.0;
    int nbytes = keylen > 32 ? 32 : keylen;
    for (int by = 0; by < nbytes; by++) {
        for (int bi = 0; bi < 8; bi++) {
            /* seed=0 — consistent with smhasher_plums */
            uint64_t seed = 0;
            buf[by] ^= (uint8_t)(1u << bi);
            uint64_t h0 = hash(buf, (size_t)keylen, seed);
            buf[by] ^= (uint8_t)(1u << bi);
            uint64_t h1 = hash(buf, (size_t)keylen, seed);
            double p = popcount(h0 ^ h1) / 64.0 * 100.0;
            if (p < worst) worst = p;
        }
    }
    free(buf);
    return worst;
}

static double chi2_test(uint64_t (*hash)(const void*,size_t,uint64_t)) {
    int bins[256] = {0};
    const int N = 256000;
    for (int i = 0; i < N; i++)
        bins[hash(&i, sizeof(i), (uint64_t)i) & 0xFF]++;
    double expected = (double)N / 256.0, chi2 = 0.0;
    for (int b = 0; b < 256; b++) {
        double d = bins[b] - expected;
        chi2 += d * d / expected;
    }
    return chi2;
}

static void bench_throughput(const hash_t *hashes, int nhash) {
    const int iterations = 2000000;
    uint8_t *buf = (uint8_t*)malloc(4096);
    if (!buf) return;

    struct { int len; const char *label; } sizes[] = {
        {4, "4B"}, {16, "16B"}, {64, "64B"}, {256, "256B"},
        {1024, "1KB"}, {4096, "4KB"}, {0, NULL}
    };

    /* Fill buffer with pseudo-random data (splitmix64) */
    {
        uint64_t seed = 0x9E3779B97F4A7C15ULL;
        for (int i = 0; i < 4096; i += 8) {
            seed += 0x9E3779B97F4A7C15ULL;
            uint64_t z = seed;
            z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
            z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
            z = z ^ (z >> 31);
            for (int j = 0; j < 8 && i + j < 4096; j++)
                buf[i + j] = (uint8_t)(z >> (j * 8));
        }
    }

    /* Warmup: run each hash once at each size to equalise thermal state */
    for (int hi = 0; hi < nhash; hi++)
        for (int si = 0; sizes[si].label; si++)
            hashes[hi].hash(buf, (size_t)sizes[si].len, 0);

    /* Build shuffled hash order for fair column ordering */
    int order[16];
    for (int i = 0; i < nhash; i++) order[i] = i;
    srand((unsigned)time(NULL));
    for (int i = nhash - 1; i > 0; i--) {
        int j = rand() % (i + 1);
        int t = order[i]; order[i] = order[j]; order[j] = t;
    }

    const int rounds = 2;  /* interleaved rounds for thermal distribution */
    const int trials = 2;  /* back-to-back runs per hash×size */

    /* Store best across rounds for each hash × size */
    double best_gb[16][8];  /* [hash][size] */
    double best_ns[16][8];
    for (int hi = 0; hi < nhash; hi++)
        for (int si = 0; sizes[si].label; si++)
            best_gb[hi][si] = 0;

    int n_sizes = 0;
    while (sizes[n_sizes].label) n_sizes++;

    for (int r = 0; r < rounds; r++) {
        /* Alternate direction each round to cancel thermal gradient:
         * even rounds: small→large, odd rounds: large→small. */
        int forward = (r % 2 == 0);
        for (int si = forward ? 0 : n_sizes - 1;
             forward ? (si < n_sizes) : (si >= 0);
             forward ? si++ : si--) {
            int len   = sizes[si].len;
            int iters = (len < 64) ? iterations * 4 : iterations;

            for (int oi = 0; oi < nhash; oi++) {
                int hi = order[oi];
                /* back-to-back runs — best filters transient throttling */
                for (int t = 0; t < trials; t++) {
                    uint64_t acc = 0;
                    acc += hashes[hi].hash(buf, (size_t)len, 0);
                    __asm__ volatile("" : "+r"(acc));

                    struct timespec t0, t1;
                    clock_gettime(CLOCK_MONOTONIC, &t0);
                    for (int i = 0; i < iters; i++)
                        acc += hashes[hi].hash(buf, (size_t)len, acc ^ (uint64_t)i);
                    clock_gettime(CLOCK_MONOTONIC, &t1);
                    __asm__ volatile("" : "+r"(acc));

                    double sec = (t1.tv_sec - t0.tv_sec)
                               + (t1.tv_nsec - t0.tv_nsec) * 1e-9;
                    double gb_s = ((double)len * iters / 1e9) / sec;
                    double ns_h = sec * 1e9 / iters;
                    if (gb_s > best_gb[hi][si]) { best_gb[hi][si] = gb_s; best_ns[hi][si] = ns_h; }
                }

                /* Brief pause between hashes — lets pipelines drain */
                { struct timespec p = {0, 10000000}; nanosleep(&p, NULL); }  /* 10 ms */
            }
        }
        /* Cooldown between rounds — allows SoC to cool */
        { struct timespec p = {2, 0}; nanosleep(&p, NULL); }
    }

    /* Print results */
    printf("%-14s", "Size");
    for (int oi = 0; oi < nhash; oi++)
        printf(" %16s", hashes[order[oi]].name);
    printf("\n%-14s", "──────");
    for (int oi = 0; oi < nhash; oi++)
        printf(" %16s", "───────");
    printf("\n");

    for (int si = 0; sizes[si].label; si++) {
        printf("%-14s", sizes[si].label);
        for (int oi = 0; oi < nhash; oi++)
            printf(" %7.2f GB/s %5.0f", best_gb[order[oi]][si], best_ns[order[oi]][si]);
        printf("\n");
    }
    free(buf);
}

static void bench_quality(const hash_t *hashes, int nhash) {
    printf("\n── Quality (seed=0, worst of 256 bit-flips) ──\n");
    printf("%-14s %8s %8s %8s\n", "Hash", "Aval256", "Aval32", "χ²");
    printf("%-14s %8s %8s %8s\n", "────", "──────", "──────", "────");

    for (int hi = 0; hi < nhash; hi++) {
        double av256 = avalanche_worst(hashes[hi].hash, 256);
        double av32  = avalanche_worst(hashes[hi].hash, 32);
        double chi2  = chi2_test(hashes[hi].hash);
        printf("%-14s %7.1f%% %7.1f%% %8.1f\n",
               hashes[hi].name, av256, av32, chi2);
    }
}

/* ═══════════════════════════════════════════════════════════════════════ */
int main(int argc, char **argv) {
    hash_t hashes[] = {
        {"R64",          r64_for_bench},
        {"PlumsHash",    plumshash},
        {"wyhash",       wyhash_for_bench},
        {"rapidhash",    rapidhash_for_bench},
        {"xxHash64",     xxhash64},
        {"FastHash64",   fasthash64},
    };
    int nhash = sizeof(hashes) / sizeof(hashes[0]);

    /* Filter by --hash name for separate-process benchmarking */
    const char *filter = NULL;
    for (int a = 1; a < argc; a++) {
        if (!strcmp(argv[a], "--hash") && a + 1 < argc)
            filter = argv[++a];
    }
    if (filter) {
        hash_t filtered[16];
        int nf = 0;
        for (int i = 0; i < nhash; i++)
            if (!strcmp(hashes[i].name, filter))
                filtered[nf++] = hashes[i];
        if (nf == 0) {
            printf("Unknown hash: %s\n", filter);
            return 1;
        }
        memcpy(hashes, filtered, nf * sizeof(hash_t));
        nhash = nf;
    }

    /* ── Sanity: verify each hash against known test vectors ── */
    printf("=== Sanity checks ===\n");
    struct { const char *hash_name; const char *input; size_t len;
             uint64_t seed; uint64_t expected; } vectors[] = {
        {"PlumsHash",   "",         0,  0, 0x0000000000000000ULL},
        {"PlumsHash",   "hello",    5,  0, 0x44EDEB18C7E91E45ULL},
        {"xxHash64",    "",         0,  0, 0xEF46DB3751D8E999ULL},
        {"xxHash64",    "hello",    5,  0, 0x26C7827D889F6DA3ULL},
        {"FastHash64",  "",         0,  0, 0x0000000000000000ULL},
        {"FastHash64",  "hello",    5,  0, 0x81E10ABD80072D82ULL},
        {"wyhash",      "",         0,  0, 0x93228A4DE0EEC5A2ULL},
        {"rapidhash",   "",         0,  0, 0x0338DC4BE2CECDAEULL},
        {"rapidhash",   "hello",    5,  0, 0x2E2D7651B45F7946ULL},
        {"R64",         "",         0,  0, 0xD4D2558E148319E1ULL},
        {"R64",         "hello",    5,  0, 0x39DEC0F5FC1C0204ULL},
        {"wyhash",      "hello",    5,  0, 0x49A593F92A7C549FULL},
    };
    int nvec = sizeof(vectors) / sizeof(vectors[0]);
    int sanity_ok = 1;
    for (int v = 0; v < nvec; v++) {
        for (int h = 0; h < nhash; h++) {
            if (strcmp(hashes[h].name, vectors[v].hash_name) != 0) continue;
            uint64_t got = hashes[h].hash(vectors[v].input, vectors[v].len, vectors[v].seed);
            if (got != vectors[v].expected) {
                printf("  FAIL: %s(\"%s\",%zu,%llu) = %016llx, expected %016llx\n",
                       hashes[h].name, vectors[v].input,
                       vectors[v].len,
                       (unsigned long long)vectors[v].seed,
                       (unsigned long long)got,
                       (unsigned long long)vectors[v].expected);
                sanity_ok = 0;
            }
            break;
        }
    }
    if (sanity_ok) printf("  All %d vectors OK\n\n", nvec);
    else { printf("  SANITY FAILURES — aborting\n"); return 1; }

    printf("=== Throughput (GB/s + ns/h) ===\n\n");
    bench_throughput(hashes, nhash);

    bench_quality(hashes, nhash);

    printf("\n(All times wall‑clock, clock_gettime.  "
           "aarch64 Cortex‑X4 / Termux unless noted.)\n");
    return 0;
}
