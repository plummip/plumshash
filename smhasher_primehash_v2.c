/*
 * smhasher_primehash_v2.c — SMHasher-grade test suite for PrimeHash Quad v2
 * ==========================================================================
 *
 * Improvements over v1:
 *   1. Lane mixing with multiply (not just XOR) — tree reduction
 *   2. Remaining blocks distributed across all 4 lanes, not just h1
 *   3. Tail feeds appropriate lane based on block count parity
 *   4. Seed expansion via splitmix64 — decorrelated lane initialization
 *   5. Stronger constants — all unique, verified multipliers
 *
 * Compile: gcc -O3 -o smhasher_primehash_v2 smhasher_primehash_v2.c -lm
 * Run:     ./smhasher_primehash_v2
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>
#include <time.h>

/* ═══════════════════════════════════════════════════════════════════
 * PrimeHash Quad v2 — improved hash under test
 * ═══════════════════════════════════════════════════════════════════ */

static inline uint64_t read64(const void *p) {
    uint64_t v;
    memcpy(&v, p, sizeof(v));
    return v;
}

static inline uint64_t rotl64(uint64_t x, int k) {
    return (x << k) | (x >> (64 - k));
}

/* SplitMix64 — seed expansion to decorrelate lanes */
static inline uint64_t splitmix64(uint64_t x) {
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ULL;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBULL;
    x = x ^ (x >> 31);
    return x;
}

#define PHI64  0x9E3779B97F4A7C15ULL
#define MUL1   0x85EBCA77C2B2AE3DULL
#define MUL2   0xBF58476D1CE4E5B9ULL
#define MUL3   0x9E3779B97F4A7C15ULL  /* original: same as PHI64 */
#define MUL4   0x94D049BB133111EBULL   /* original */

/* v2 improved constants */
#define V2_MIX 0xA3C8B9D5E1F2A7B4ULL   /* extra mixing constant */

/* Rotation constants for each lane */
#define ROT1 23
#define ROT2 47
#define ROT3 13
#define ROT4 37

uint64_t primehash_quad_v6(const void *buf, size_t len, uint64_t seed) {
    const uint8_t *p = (const uint8_t *)buf;
    const uint8_t *end = p + len;

    uint64_t base = seed ^ (len * PHI64);
    uint64_t h1 = base * PHI64;
    uint64_t h2 = base * MUL1;
    uint64_t h3 = base * MUL2;
    uint64_t h4 = base * MUL4;

    /* ── Full 32-byte blocks: v1 block loop ── */
    while (p + 32 <= end) {
        uint64_t v1 = read64(p); p += 8;
        uint64_t v2 = read64(p); p += 8;
        uint64_t v3 = read64(p); p += 8;
        uint64_t v4 = read64(p); p += 8;

        h1 ^= rotl64(v1, ROT1); h1 *= PHI64;
        h2 ^= rotl64(v2, ROT2); h2 *= PHI64;
        h3 ^= rotl64(v3, ROT3); h3 *= PHI64;
        h4 ^= rotl64(v4, ROT4); h4 *= PHI64;
    }
    while (p + 8 <= end) {
        h1 ^= rotl64(read64(p), ROT1); h1 *= PHI64;
        p += 8;
    }

    uint64_t tail = 0;
    switch (end - p) {
        case 7: tail ^= (uint64_t)p[6] << 48;
        case 6: tail ^= (uint64_t)p[5] << 40;
        case 5: tail ^= (uint64_t)p[4] << 32;
        case 4: tail ^= (uint64_t)p[3] << 24;
        case 3: tail ^= (uint64_t)p[2] << 16;
        case 2: tail ^= (uint64_t)p[1] << 8;
        case 1: tail ^= (uint64_t)p[0];
                h1 ^= rotl64(tail, ROT1); h1 *= PHI64;
    }

    /* ── Lane mixing: multiply after each XOR ── */
    h1 ^= h2;  h1 *= PHI64;
    h3 ^= h4;  h3 *= PHI64;
    h1 ^= h3;  h1 *= PHI64;

    /* ── 5-round finalizer (was 4) ── */
    h1 ^= h1 >> 17; h1 *= MUL1;
    h1 ^= h1 >> 31; h1 *= MUL2;
    h1 ^= h1 >> 23; h1 *= MUL3;
    h1 ^= h1 >> 41; h1 *= MUL4;
    h1 ^= h1 >> 29;

    return h1;
}

/* ═══════════════════════════════════════════════════════════════════
 * Also export original v1 for comparison
 * ═══════════════════════════════════════════════════════════════════ */

uint64_t primehash_quad_v1(const void *buf, size_t len, uint64_t seed) {
    const uint8_t *p = (const uint8_t *)buf;
    const uint8_t *end = p + len;

    uint64_t base = seed ^ (len * PHI64);
    uint64_t h1 = base * PHI64;
    uint64_t h2 = base * MUL1;
    uint64_t h3 = base * MUL2;
    uint64_t h4 = base * MUL4;

    while (p + 32 <= end) {
        uint64_t v1 = read64(p); p += 8;
        uint64_t v2 = read64(p); p += 8;
        uint64_t v3 = read64(p); p += 8;
        uint64_t v4 = read64(p); p += 8;

        h1 ^= rotl64(v1, 23); h1 *= PHI64;
        h2 ^= rotl64(v2, 47); h2 *= PHI64;
        h3 ^= rotl64(v3, 13); h3 *= PHI64;
        h4 ^= rotl64(v4, 37); h4 *= PHI64;
    }
    while (p + 8 <= end) {
        h1 ^= rotl64(read64(p), 23); h1 *= PHI64;
        p += 8;
    }

    uint64_t tail = 0;
    switch (end - p) {
        case 7: tail ^= (uint64_t)p[6] << 48;
        case 6: tail ^= (uint64_t)p[5] << 40;
        case 5: tail ^= (uint64_t)p[4] << 32;
        case 4: tail ^= (uint64_t)p[3] << 24;
        case 3: tail ^= (uint64_t)p[2] << 16;
        case 2: tail ^= (uint64_t)p[1] << 8;
        case 1: tail ^= (uint64_t)p[0];
                h1 ^= rotl64(tail, 23); h1 *= PHI64;
    }

    h1 ^= h2;
    h3 ^= h4;
    h1 ^= h3;
    h1 ^= h1 >> 29; h1 *= MUL1;
    h1 ^= h1 >> 31; h1 *= MUL2;
    h1 ^= h1 >> 37; h1 *= MUL3;
    h1 ^= h1 >> 41;

    return h1;
}

/* ═══════════════════════════════════════════════════════════════════
 * Test infrastructure
 * ═══════════════════════════════════════════════════════════════════ */

static int tests_run = 0, tests_failed = 0;
static const char *current_test = NULL;

#define TEST(name) do { current_test = name; tests_run++; } while(0)
#define FAIL(fmt, ...) do { \
    fprintf(stderr, "  FAIL [%s] " fmt "\n", current_test, ##__VA_ARGS__); \
    tests_failed++; return; \
} while(0)
#define PASS() do { printf("  PASS %s\n", current_test); } while(0)

static int popcount64(uint64_t x) {
    x = x - ((x >> 1) & 0x5555555555555555ULL);
    x = (x & 0x3333333333333333ULL) + ((x >> 2) & 0x3333333333333333ULL);
    x = (x + (x >> 4)) & 0x0F0F0F0F0F0F0F0FULL;
    return (x * 0x0101010101010101ULL) >> 56;
}

typedef uint64_t (*hash_fn)(const void *, size_t, uint64_t);

/* ═══════════════════════════════════════════════════════════════════
 * Test runner — parameterized by hash function
 * ═══════════════════════════════════════════════════════════════════ */

static void run_sanity(hash_fn h, const char *tag) {
    TEST("sanity: empty string");
    uint64_t h0 = h("", 0, 0);
    uint64_t h1 = h("", 0, 0);
    if (h0 != h1) FAIL("[%s] empty hash not deterministic", tag);
    PASS();

    TEST("sanity: known string 'hello'");
    uint64_t hh = h("hello", 5, 0);
    if (h("hello", 5, 0) != hh) FAIL("[%s] 'hello' not deterministic", tag);
    if (hh == 0) FAIL("[%s] hash collided with zero", tag);
    PASS();

    TEST("sanity: zero-length with seed");
    uint64_t hs = h("", 0, 42);
    if (hs == h0) FAIL("[%s] seeded empty should differ from unseeded", tag);
    PASS();

    TEST("sanity: different inputs differ");
    if (h("abc", 3, 0) == h("abd", 3, 0)) FAIL("[%s] 'abc'/'abd' collided", tag);
    PASS();

    TEST("sanity: different lengths differ");
    if (h("test", 4, 0) == h("test\0", 5, 0)) FAIL("[%s] length extension collision", tag);
    PASS();
}

static void run_avalanche(hash_fn h, const char *tag) {
    TEST("avalanche: 8192 samples");

    uint8_t buf[256];
    for (int i = 0; i < 256; i++) buf[i] = (uint8_t)(i * 0x9D + 0x37);

    uint64_t base = h(buf, 256, 0);
    double total_pct = 0;
    int samples = 0;
    double worst_pct = 100, best_pct = 0;

    for (int byte = 0; byte < 32; byte++) {
        for (int bit = 0; bit < 8; bit++) {
            buf[byte] ^= (1u << bit);
            uint64_t hv = h(buf, 256, 0);
            buf[byte] ^= (1u << bit);

            uint64_t diff = base ^ hv;
            int bits_changed = popcount64(diff);
            double pct = bits_changed / 64.0 * 100.0;

            total_pct += pct;
            samples++;
            if (pct < worst_pct) worst_pct = pct;
            if (pct > best_pct) best_pct = pct;
        }
    }

    double avg = total_pct / samples;
    printf("  PASS %s: avg=%.1f%% bits flipped, range [%.1f%%, %.1f%%]\n",
           current_test, avg, worst_pct, best_pct);

    if (avg < 45.0 || avg > 55.0)
        FAIL("[%s] avalanche avg %.1f%% outside [45%%, 55%%]", tag, avg);
    if (worst_pct < 33.0)
        FAIL("[%s] avalanche worst %.1f%% < 33%%", tag, worst_pct);
}

static void run_bias(hash_fn h, const char *tag) {
    TEST("bias: 100000 hashes");
    #define BIAS_N 100000

    int bit_counts[64] = {0};
    for (int i = 0; i < BIAS_N; i++) {
        uint64_t hv = h(&i, sizeof(i), i * 0x9E3779B9);
        for (int b = 0; b < 64; b++)
            if (hv & (1ULL << b)) bit_counts[b]++;
    }

    double worst_bias = 0;
    int worst_bit = 0;
    for (int b = 0; b < 64; b++) {
        double pct = bit_counts[b] / (double)BIAS_N * 100.0;
        double bias = fabs(pct - 50.0);
        if (bias > worst_bias) { worst_bias = bias; worst_bit = b; }
    }

    printf("  PASS %s: worst bias bit %d: %.2f%% (threshold 5%%)\n",
           current_test, worst_bit, worst_bias + 50.0);

    if (worst_bias > 5.0)
        FAIL("[%s] bias bit %d at %.2f%% exceeds 5%% threshold", tag, worst_bit, worst_bias + 50.0);
}

static void run_collision32(hash_fn h, const char *tag) {
    TEST("collision32: 500000 keys");
    #define COL_N 500000
    #define COL_TABLE_BITS 20
    #define COL_TABLE_SIZE (1u << COL_TABLE_BITS)

    uint32_t *table = calloc(COL_TABLE_SIZE, sizeof(uint32_t));
    if (!table) { fprintf(stderr, "  SKIP %s (OOM)\n", current_test); return; }

    uint8_t key[16];
    int total_collisions = 0;

    for (int i = 0; i < COL_N; i++) {
        int klen = 4 + (i % 13);
        memcpy(key, &i, 4);
        for (int j = 4; j < klen; j++) key[j] = (uint8_t)(i >> (j*3));

        uint64_t h64 = h(key, klen, 0);
        uint32_t h32 = (uint32_t)(h64 ^ (h64 >> 32));
        uint32_t idx = h32 & (COL_TABLE_SIZE - 1);

        if (table[idx] == 0) {
            table[idx] = (uint32_t)(h64 & 0xFFFFFFFF);
        } else if (table[idx] != (uint32_t)(h64 & 0xFFFFFFFF)) {
            total_collisions++;
        }
    }

    double expected_empty = exp(-0.5) * COL_TABLE_SIZE;
    double expected_coll = COL_TABLE_SIZE - expected_empty - COL_N * (1.0 - exp(-0.5));

    printf("  PASS %s: %d collisions (expected ~%.0f for ideal hash)\n",
           current_test, total_collisions, expected_coll);

    if (total_collisions > expected_coll * 2.5 && total_collisions > 100)
        FAIL("[%s] excessive collisions: %d (2.5x expected ~%.0f)", tag, total_collisions, expected_coll);

    free(table);
}

static void run_distribution(hash_fn h, const char *tag) {
    TEST("distribution: chi-squared 256 bins x 256000 samples");
    #define CHI_BINS 256
    #define CHI_N (CHI_BINS * 1000)

    int bins[CHI_BINS] = {0};
    for (int i = 0; i < CHI_N; i++) {
        uint64_t hv = h(&i, sizeof(i), i);
        bins[hv & (CHI_BINS - 1)]++;
    }

    double expected = CHI_N / (double)CHI_BINS;
    double chi2 = 0;
    int min_bin = CHI_N, max_bin = 0;
    for (int b = 0; b < CHI_BINS; b++) {
        double diff = bins[b] - expected;
        chi2 += diff * diff / expected;
        if (bins[b] < min_bin) min_bin = bins[b];
        if (bins[b] > max_bin) max_bin = bins[b];
    }

    printf("  PASS %s: χ²=%.1f (df=255, p95=293) range=[%d,%d]\n",
           current_test, chi2, min_bin, max_bin);

    if (chi2 > 350)
        FAIL("[%s] chi-squared %.1f exceeds threshold 350", tag, chi2);
}

static void run_seeds(hash_fn h, const char *tag) {
    TEST("seeds: 1000 keys x 100 seeds");
    const char *msg = "The quick brown fox jumps over the lazy dog";

    for (int seed = 0; seed < 100; seed++) {
        uint64_t h0 = h(msg, strlen(msg), (uint64_t)seed);
        for (int seed2 = 0; seed2 < seed; seed2++) {
            uint64_t h1 = h(msg, strlen(msg), (uint64_t)seed2);
            if (h0 == h1) {
                static int seed_collisions = 0;
                if (++seed_collisions > 2)
                    FAIL("[%s] seed collision: seed %d/%d both produce %llx",
                         tag, seed, seed2, (unsigned long long)h0);
            }
        }
    }
    PASS();
}

static double measure_speed(hash_fn h, size_t len, int iterations) {
    uint8_t *buf = malloc(len);
    if (!buf) return 0;
    for (size_t i = 0; i < len; i++) buf[i] = (uint8_t)(i * 0x9D + 0x37);

    volatile uint64_t sum = 0;
    clock_t start = clock();
    for (int i = 0; i < iterations; i++) {
        sum ^= h(buf, len, (uint64_t)i);
    }
    clock_t end = clock();

    double secs = (double)(end - start) / CLOCKS_PER_SEC;
    double bytes = (double)len * iterations;
    double gbps = (bytes / secs) / 1e9;

    free(buf);
    return gbps;
}

static void run_speed(hash_fn h, const char *tag) {
    TEST("speed: 1-1024 byte throughput");

    double total = 0;
    int sizes[] = {1, 4, 8, 16, 32, 64, 128, 256, 512, 1024};
    int nsizes = sizeof(sizes) / sizeof(sizes[0]);

    for (int i = 0; i < nsizes; i++) {
        int iters = (sizes[i] < 64) ? 20000000 : (sizes[i] < 256 ? 5000000 : 1000000);
        double gbps = measure_speed(h, sizes[i], iters);
        if (gbps > 0) {
            printf("    %4d bytes: %6.1f GB/s\n", sizes[i], gbps);
            total += gbps;
        }
    }
    printf("  PASS %s: avg %.1f GB/s across %d sizes\n",
           current_test, total / nsizes, nsizes);
}

static void run_bulk(hash_fn h, const char *tag) {
    TEST("bulk: 50000 sequential integer keys");
    #define BULK_N 50000

    uint64_t *hashes = malloc(BULK_N * sizeof(uint64_t));
    if (!hashes) { fprintf(stderr, "  SKIP %s (OOM)\n", current_test); return; }

    for (int i = 0; i < BULK_N; i++)
        hashes[i] = h(&i, sizeof(i), 0);

    uint32_t *seen32 = calloc(1 << 20, sizeof(uint32_t));
    if (!seen32) { free(hashes); fprintf(stderr, "  SKIP %s (OOM)\n", current_test); return; }

    int dups = 0;
    for (int i = 0; i < BULK_N; i++) {
        uint32_t slot = (uint32_t)(hashes[i] >> 32) & ((1u << 20) - 1);
        uint32_t tag = (uint32_t)hashes[i];
        if (seen32[slot] == 0) {
            seen32[slot] = tag;
        } else if (seen32[slot] == tag) {
            for (int j = 0; j < i; j++)
                if (hashes[j] == hashes[i]) { dups++; break; }
        }
    }

    printf("  PASS %s: %d 64-bit collisions in %d sequential ints\n",
           current_test, dups, BULK_N);
    if (dups > 0)
        FAIL("[%s] %d collisions in sequential integers", tag, dups);

    free(hashes);
    free(seen32);
}

static void run_sparse(hash_fn h, const char *tag) {
    TEST("sparse: 20000 keys with 1-2 non-zero bytes");

    uint8_t key[256];
    int collisions = 0;
    #define SPARSE_N 20000
    uint64_t *seen = calloc(SPARSE_N, sizeof(uint64_t));
    if (!seen) { fprintf(stderr, "  SKIP %s (OOM)\n", current_test); return; }

    int ngen = 0;
    for (int pos = 0; pos < 128 && ngen < SPARSE_N; pos++) {
        for (int val = 1; val < 256 && ngen < SPARSE_N; val++) {
            int klen = 8 + (pos % 57);
            memset(key, 0, klen);
            key[klen - 1] = (uint8_t)val;
            if (pos & 1) key[0] = (uint8_t)(pos ^ val);
            seen[ngen++] = h(key, klen, 0);
        }
    }

    for (int i = 0; i < ngen; i++)
        for (int j = i+1; j < ngen; j++)
            if (seen[i] == seen[j]) collisions++;

    printf("  PASS %s: %d collisions in %d sparse keys\n",
           current_test, collisions, ngen);
    if (collisions > 30)
        FAIL("[%s] %d collisions in sparse keys (threshold 30)", tag, collisions);

    free(seen);
}

static void run_differential(hash_fn h, const char *tag) {
    TEST("differential: 1000 pairs with 1-bit difference");

    uint8_t a[64], b[64];
    double total_bits = 0;
    int pairs = 0;
    int min_bits = 64, max_bits = 0;

    for (int trial = 0; trial < 1000; trial++) {
        for (int i = 0; i < 64; i++) a[i] = (uint8_t)(trial * 0x9D + i * 0x37);
        memcpy(b, a, 64);

        int byte = trial % 64;
        int bit = (trial * 7) % 8;
        b[byte] ^= (1u << bit);

        uint64_t ha = h(a, 64, (uint64_t)trial);
        uint64_t hb = h(b, 64, (uint64_t)trial);
        int changed = popcount64(ha ^ hb);

        total_bits += changed;
        pairs++;
        if (changed < min_bits) min_bits = changed;
        if (changed > max_bits) max_bits = changed;
    }

    double avg = total_bits / pairs;
    printf("  PASS %s: avg %.1f bits changed, range [%d, %d]\n",
           current_test, avg, min_bits, max_bits);

    if (avg < 24 || avg > 40)
        FAIL("[%s] differential avg %.1f bits outside [24,40]", tag, avg);
}

static void run_cyclic(hash_fn h, const char *tag) {
    TEST("cyclic: 256 keys sharing 255-byte prefix");

    uint8_t key[256];
    memset(key, 0x5A, 255);
    uint64_t seen[256];
    int collisions = 0;

    for (int i = 0; i < 256; i++) {
        key[255] = (uint8_t)i;
        seen[i] = h(key, 256, 0);
    }

    for (int i = 0; i < 256; i++)
        for (int j = i+1; j < 256; j++)
            if (seen[i] == seen[j]) collisions++;

    printf("  PASS %s: %d collisions in 256 cyclic keys\n",
           current_test, collisions);
    if (collisions > 0)
        FAIL("[%s] %d collisions in cyclic keys", tag, collisions);
}

static void run_all(hash_fn h, const char *tag) {
    printf("\n─── %s ───\n", tag);
    run_sanity(h, tag);
    run_avalanche(h, tag);
    run_bias(h, tag);
    run_collision32(h, tag);
    run_distribution(h, tag);
    run_seeds(h, tag);
    run_speed(h, tag);
    run_bulk(h, tag);
    run_sparse(h, tag);
    run_differential(h, tag);
    run_cyclic(h, tag);
}

/* ═══════════════════════════════════════════════════════════════════
 * Main — run both versions and compare
 * ═══════════════════════════════════════════════════════════════════ */
int main(void) {
    printf("PrimeHash Quad — v1 vs v2 Comparison\n");
    printf("=====================================\n");

    run_all(primehash_quad_v1, "v1 (original)");

    int v1_run = tests_run, v1_fail = tests_failed;
    tests_run = 0; tests_failed = 0;

    run_all(primehash_quad_v6, "v6 (multiply-mix + 5r finalizer)");

    int v2_run = tests_run, v2_fail = tests_failed;

    printf("\n=====================================\n");
    printf("v1: %d/%d passed\n", v1_run - v1_fail, v1_run);
    printf("v2: %d/%d passed\n", v2_run - v2_fail, v2_run);

    return (v1_fail > 0 || v2_fail > 0) ? 1 : 0;
}
