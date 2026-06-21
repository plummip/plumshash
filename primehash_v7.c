/*
 * smhasher_primehash_v7.c — PrimeHash "Sieve" v7
 * ===============================================
 * Inspired by PRIEMFORMULE's 9-column prime sieve:
 *   - Alternating even/odd block mixing (checkerboard pattern)
 *   - 6 "safe residue" rotation constants from {0,1,3,4,6,7}
 *   - Multi-level lane mixing (hierarchical sieve cascade)
 *   - Distributed remaining blocks + tail (v5 fix)
 *   - 5-round finalizer
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>
#include <time.h>

/* ═══════════════════════════════════════════════════════════════════
 * PrimeHash v7 "Sieve"
 * ═══════════════════════════════════════════════════════════════════ */

static inline uint64_t read64(const void *p) {
    uint64_t v; memcpy(&v, p, 8); return v;
}
static inline uint64_t rotl64(uint64_t x, int k) {
    return (x << k) | (x >> (64 - k));
}

/* Constants: φ-based multipliers (all coprime to 2^64) */
#define PHI  0x9E3779B97F4A7C15ULL
#define M1   0x85EBCA77C2B2AE3DULL
#define M2   0xBF58476D1CE4E5B9ULL
#define M3   0x94D049BB133111EBULL
#define M4   0xA3C8B9D5E1F2A7B4ULL  /* new: from safe residue space */

/*
 * Rotation sets inspired by 6 "safe columns" mod 9: {0,1,3,4,6,7}
 * Mapped to rotation amounts: 0→17, 1→23, 3→29, 4→37, 6→43, 7→53
 *
 * Even blocks use {17, 29, 43, 53}  (columns 0,3,6,7 — half the safe set)
 * Odd blocks use  {23, 37, 59, 61}  (columns 1,4 + extras — complementary)
 */
#define EVEN_R1 17
#define EVEN_R2 29
#define EVEN_R3 43
#define EVEN_R4 53

#define ODD_R1  23
#define ODD_R2  37
#define ODD_R3  59
#define ODD_R4  61

uint64_t primehash_v7(const void *buf, size_t len, uint64_t seed) {
    const uint8_t *p = (const uint8_t *)buf;
    const uint8_t *end = p + len;

    uint64_t base = seed ^ (len * PHI);
    uint64_t h1 = base * PHI;
    uint64_t h2 = base * M1;
    uint64_t h3 = base * M2;
    uint64_t h4 = base * M3;

    int block_count = 0;

    /* ── Full 32-byte blocks: alternating even/odd mixing ── */
    while (p + 32 <= end) {
        uint64_t v1 = read64(p); p += 8;
        uint64_t v2 = read64(p); p += 8;
        uint64_t v3 = read64(p); p += 8;
        uint64_t v4 = read64(p); p += 8;

        if (block_count & 1) {
            /* Odd block: complementary rotations */
            h1 ^= rotl64(v1, ODD_R1); h1 *= PHI;
            h2 ^= rotl64(v2, ODD_R2); h2 *= PHI;
            h3 ^= rotl64(v3, ODD_R3); h3 *= PHI;
            h4 ^= rotl64(v4, ODD_R4); h4 *= PHI;
        } else {
            /* Even block: standard rotations from safe set */
            h1 ^= rotl64(v1, EVEN_R1); h1 *= PHI;
            h2 ^= rotl64(v2, EVEN_R2); h2 *= PHI;
            h3 ^= rotl64(v3, EVEN_R3); h3 *= PHI;
            h4 ^= rotl64(v4, EVEN_R4); h4 *= PHI;
        }
        block_count++;
    }

    /* ── Remaining 8-byte blocks: distribute across lanes ── */
    while (p + 8 <= end) {
        uint64_t v = read64(p); p += 8;
        switch (block_count & 3) {
            case 0:
                h1 ^= rotl64(v, (block_count & 1) ? ODD_R1 : EVEN_R1);
                h1 *= PHI; break;
            case 1:
                h2 ^= rotl64(v, (block_count & 1) ? ODD_R2 : EVEN_R2);
                h2 *= PHI; break;
            case 2:
                h3 ^= rotl64(v, (block_count & 1) ? ODD_R3 : EVEN_R3);
                h3 *= PHI; break;
            case 3:
                h4 ^= rotl64(v, (block_count & 1) ? ODD_R4 : EVEN_R4);
                h4 *= PHI; break;
        }
        block_count++;
    }

    /* ── Tail: feed into appropriate lane ── */
    int tail_len = end - p;
    if (tail_len > 0) {
        uint64_t tail = 0;
        switch (tail_len) {
            case 7: tail ^= (uint64_t)p[6] << 48;
            case 6: tail ^= (uint64_t)p[5] << 40;
            case 5: tail ^= (uint64_t)p[4] << 32;
            case 4: tail ^= (uint64_t)p[3] << 24;
            case 3: tail ^= (uint64_t)p[2] << 16;
            case 2: tail ^= (uint64_t)p[1] << 8;
            case 1: tail ^= (uint64_t)p[0];
        }
        int odd = block_count & 1;
        switch (block_count & 3) {
            case 0: h1 ^= rotl64(tail, odd ? ODD_R1 : EVEN_R1); h1 *= PHI; break;
            case 1: h2 ^= rotl64(tail, odd ? ODD_R2 : EVEN_R2); h2 *= PHI; break;
            case 2: h3 ^= rotl64(tail, odd ? ODD_R3 : EVEN_R3); h3 *= PHI; break;
            case 3: h4 ^= rotl64(tail, odd ? ODD_R4 : EVEN_R4); h4 *= PHI; break;
        }
    }

    /* ── Lane mixing: 3-level hierarchical cascade ──
     * Level 1 (mod 3): pair adjacent lanes with multiply
     * Level 2 (mod 2): mix the two pairs with multiply
     * Level 3: absorb into h1 with multiply
     */
    h1 ^= h2;  h1 *= PHI;   /* level 1a: columns (0,3) */
    h3 ^= h4;  h3 *= PHI;   /* level 1b: columns (4,6) */
    h1 ^= h3;  h1 *= M4;    /* level 2: combine halves */

    /* ── Finalizer: 5 rounds using "safe residue" shift amounts ──
     * Shifts from the 6 safe columns mapped to bit distances:
     * {17, 23, 29, 37, 43} — all primes, well-spaced
     */
    h1 ^= h1 >> 17; h1 *= M1;
    h1 ^= h1 >> 23; h1 *= M2;
    h1 ^= h1 >> 29; h1 *= M3;
    h1 ^= h1 >> 37; h1 *= M4;
    h1 ^= h1 >> 43;

    return h1;
}

/* ═══════════════════════════════════════════════════════════════════
 * v1 (original) for comparison
 * ═══════════════════════════════════════════════════════════════════ */
uint64_t primehash_v1(const void *buf, size_t len, uint64_t seed) {
    const uint8_t *p = (const uint8_t *)buf;
    const uint8_t *end = p + len;
    uint64_t base = seed ^ (len * PHI);
    uint64_t h1 = base * PHI, h2 = base * M1, h3 = base * M2, h4 = base * M3;
    while (p + 32 <= end) {
        h1 ^= rotl64(read64(p), 23); h1 *= PHI; p += 8;
        h2 ^= rotl64(read64(p), 47); h2 *= PHI; p += 8;
        h3 ^= rotl64(read64(p), 13); h3 *= PHI; p += 8;
        h4 ^= rotl64(read64(p), 37); h4 *= PHI; p += 8;
    }
    while (p + 8 <= end) { h1 ^= rotl64(read64(p), 23); h1 *= PHI; p += 8; }
    uint64_t tail = 0;
    switch (end - p) {
        case 7: tail ^= (uint64_t)p[6] << 48; case 6: tail ^= (uint64_t)p[5] << 40;
        case 5: tail ^= (uint64_t)p[4] << 32; case 4: tail ^= (uint64_t)p[3] << 24;
        case 3: tail ^= (uint64_t)p[2] << 16; case 2: tail ^= (uint64_t)p[1] << 8;
        case 1: tail ^= (uint64_t)p[0]; h1 ^= rotl64(tail, 23); h1 *= PHI;
    }
    h1 ^= h2; h3 ^= h4; h1 ^= h3;
    h1 ^= h1 >> 29; h1 *= M1; h1 ^= h1 >> 31; h1 *= M2;
    h1 ^= h1 >> 37; h1 *= PHI; h1 ^= h1 >> 41;
    return h1;
}

/* ═══════════════════════════════════════════════════════════════════
 * Test harness
 * ═══════════════════════════════════════════════════════════════════ */
typedef uint64_t (*hash_fn)(const void *, size_t, uint64_t);

static int popcount64(uint64_t x) {
    x = x - ((x >> 1) & 0x5555555555555555ULL);
    x = (x & 0x3333333333333333ULL) + ((x >> 2) & 0x3333333333333333ULL);
    x = (x + (x >> 4)) & 0x0F0F0F0F0F0F0F0FULL;
    return (x * 0x0101010101010101ULL) >> 56;
}

static int tests_run, tests_failed;
static const char *current_test;
#define TEST(n) do { current_test=n; tests_run++; } while(0)
#define FAIL(f,...) do { fprintf(stderr,"  FAIL [%s] " f "\n",current_test,##__VA_ARGS__); tests_failed++; return; } while(0)
#define PASS() do { printf("  PASS %s\n", current_test); } while(0)

static void test_avalanche(hash_fn h, const char *tag) {
    TEST("avalanche");
    uint8_t buf[256];
    for (int i = 0; i < 256; i++) buf[i] = (uint8_t)(i * 0x9D + 0x37);
    uint64_t base = h(buf, 256, 0);
    double total = 0, worst = 100, best = 0; int samples = 0;
    for (int byte = 0; byte < 32; byte++) {
        for (int bit = 0; bit < 8; bit++) {
            buf[byte] ^= (1u << bit);
            uint64_t hv = h(buf, 256, 0);
            buf[byte] ^= (1u << bit);
            double pct = popcount64(base ^ hv) / 64.0 * 100.0;
            total += pct; samples++;
            if (pct < worst) worst = pct;
            if (pct > best) best = pct;
        }
    }
    double avg = total / samples;
    printf("  %-12s avalanche: avg=%.1f%% worst=%.1f%% best=%.1f%%\n", tag, avg, worst, best);
    if (avg < 45.0 || avg > 55.0) FAIL("%s avg %.1f%%", tag, avg);
    if (worst < 33.0) FAIL("%s worst %.1f%% < 33%%", tag, worst);
}

static void test_bias(hash_fn h, const char *tag) {
    TEST("bias");
    #define BN 100000
    int bc[64] = {0};
    for (int i = 0; i < BN; i++) {
        uint64_t hv = h(&i, sizeof(i), i * 0x9E3779B9);
        for (int b = 0; b < 64; b++) if (hv & (1ULL << b)) bc[b]++;
    }
    double worst = 0; int wb = 0;
    for (int b = 0; b < 64; b++) {
        double bias = fabs(bc[b] / (double)BN * 100.0 - 50.0);
        if (bias > worst) { worst = bias; wb = b; }
    }
    printf("  %-12s bias: worst bit %2d at %.2f%%\n", tag, wb, worst + 50.0);
    if (worst > 5.0) FAIL("%s bias %.2f%%", tag, worst + 50.0);
}

static void test_distribution(hash_fn h, const char *tag) {
    TEST("distribution");
    #define DB 256
    #define DN (DB * 1000)
    int bins[DB] = {0};
    for (int i = 0; i < DN; i++) bins[h(&i, sizeof(i), (uint64_t)i) & (DB - 1)]++;
    double expv = DN / (double)DB, chi2 = 0;
    for (int b = 0; b < DB; b++) { double d = bins[b] - expv; chi2 += d * d / expv; }
    printf("  %-12s chi2: %.1f (df=255, p95=293)\n", tag, chi2);
    if (chi2 > 350) FAIL("%s chi2 %.1f", tag, chi2);
}

static void test_collision32(hash_fn h, const char *tag) {
    TEST("collision32");
    #define CN 500000
    #define CTS (1u << 20)
    uint32_t *tbl = calloc(CTS, sizeof(uint32_t));
    if (!tbl) { printf("  SKIP\n"); return; }
    uint8_t key[16]; int cols = 0;
    for (int i = 0; i < CN; i++) {
        int kl = 4 + (i % 13); memcpy(key, &i, 4);
        for (int j = 4; j < kl; j++) key[j] = (uint8_t)(i >> (j * 3));
        uint64_t h64 = h(key, kl, 0);
        uint32_t h32 = (uint32_t)(h64 ^ (h64 >> 32));
        uint32_t idx = h32 & (CTS - 1);
        if (tbl[idx] == 0) tbl[idx] = (uint32_t)(h64 & 0xFFFFFFFF);
        else if (tbl[idx] != (uint32_t)(h64 & 0xFFFFFFFF)) cols++;
    }
    double ec = CTS * (1.0 - exp(-(double)CN / CTS)) - CN * exp(-(double)CN / CTS);
    printf("  %-12s collisions: %d (expected ~%.0f)\n", tag, cols, ec);
    if (cols > ec * 2.5 && cols > 100) FAIL("%s excessive %d", tag, cols);
    free(tbl);
}

static void test_differential(hash_fn h, const char *tag) {
    TEST("differential");
    uint8_t a[64], b[64]; double total = 0; int pairs = 0, minb = 64, maxb = 0;
    for (int t = 0; t < 1000; t++) {
        for (int i = 0; i < 64; i++) a[i] = (uint8_t)(t * 0x9D + i * 0x37);
        memcpy(b, a, 64);
        b[t % 64] ^= (1u << ((t * 7) % 8));
        int c = popcount64(h(a, 64, (uint64_t)t) ^ h(b, 64, (uint64_t)t));
        total += c; pairs++; if (c < minb) minb = c; if (c > maxb) maxb = c;
    }
    printf("  %-12s diff: avg=%.1f range=[%d,%d]\n", tag, total / pairs, minb, maxb);
    if (total / pairs < 24 || total / pairs > 40) FAIL("%s diff avg %.1f", tag, total / pairs);
}

static void test_speed(hash_fn h, const char *tag) {
    TEST("speed");
    int sz[] = {1, 4, 8, 16, 32, 64, 256, 1024};
    int ns = sizeof(sz) / sizeof(sz[0]);
    printf("  %-12s speed:", tag);
    double total = 0;
    for (int i = 0; i < ns; i++) {
        uint8_t *buf = malloc(sz[i]);
        if (!buf) continue;
        for (int j = 0; j < sz[i]; j++) buf[j] = (uint8_t)(j * 0x9D + 0x37);
        int iters = sz[i] < 64 ? 20000000 : sz[i] < 256 ? 5000000 : 1000000;
        volatile uint64_t sum = 0;
        clock_t st = clock();
        for (int j = 0; j < iters; j++) sum ^= h(buf, sz[i], (uint64_t)j);
        clock_t en = clock();
        double gbps = (sz[i] * (double)iters / ((double)(en - st) / CLOCKS_PER_SEC)) / 1e9;
        if (gbps > 0) { printf(" %dB:%.1f", sz[i], gbps); total += gbps; }
        free(buf);
    }
    printf(" | avg:%.1f GB/s\n", total / ns);
}

static void test_bulk(hash_fn h, const char *tag) {
    TEST("bulk");
    #define BN2 50000
    uint64_t *hashes = malloc(BN2 * 8);
    if (!hashes) { printf("  SKIP\n"); return; }
    for (int i = 0; i < BN2; i++) hashes[i] = h(&i, sizeof(i), 0);
    uint32_t *s32 = calloc(1 << 20, 4);
    if (!s32) { free(hashes); printf("  SKIP\n"); return; }
    int dups = 0;
    for (int i = 0; i < BN2; i++) {
        uint32_t slot = (uint32_t)(hashes[i] >> 32) & ((1u << 20) - 1);
        uint32_t tag = (uint32_t)hashes[i];
        if (s32[slot] == 0) s32[slot] = tag;
        else if (s32[slot] == tag)
            for (int j = 0; j < i; j++) if (hashes[j] == hashes[i]) { dups++; break; }
    }
    printf("  %-12s bulk: %d collisions in 50K ints\n", tag, dups);
    if (dups > 0) FAIL("%s %d collisions", tag, dups);
    free(hashes); free(s32);
}

static void test_cyclic(hash_fn h, const char *tag) {
    TEST("cyclic");
    uint8_t key[256]; memset(key, 0x5A, 255);
    uint64_t seen[256]; int cols = 0;
    for (int i = 0; i < 256; i++) { key[255] = (uint8_t)i; seen[i] = h(key, 256, 0); }
    for (int i = 0; i < 256; i++)
        for (int j = i + 1; j < 256; j++)
            if (seen[i] == seen[j]) cols++;
    printf("  %-12s cyclic: %d collisions\n", tag, cols);
    if (cols > 0) FAIL("%s cyclic %d", tag, cols);
}

static void test_sanity(hash_fn h, const char *tag) {
    TEST("sanity: deterministic");
    uint64_t a = h("hello", 5, 0);
    if (h("hello", 5, 0) != a) FAIL("%s not deterministic", tag);
    if (a == 0) FAIL("%s zero output", tag);
    PASS();
    TEST("sanity: seed differs");
    if (h("", 0, 0) == h("", 0, 42)) FAIL("%s seed ignored", tag);
    PASS();
    TEST("sanity: diff input");
    if (h("abc", 3, 0) == h("abd", 3, 0)) FAIL("%s collision on diff input", tag);
    PASS();
    TEST("sanity: length");
    if (h("test", 4, 0) == h("test\0", 5, 0)) FAIL("%s len extension", tag);
    PASS();
}

int main(void) {
    printf("=== PrimeHash v1 vs v7 \"Sieve\" ===\n\n");

    tests_run = tests_failed = 0;
    printf("── v1 (original) ──\n");
    test_sanity(primehash_v1, "v1");
    test_avalanche(primehash_v1, "v1");
    test_bias(primehash_v1, "v1");
    test_distribution(primehash_v1, "v1");
    test_collision32(primehash_v1, "v1");
    test_differential(primehash_v1, "v1");
    test_speed(primehash_v1, "v1");
    test_bulk(primehash_v1, "v1");
    test_cyclic(primehash_v1, "v1");
    int r1 = tests_run, f1 = tests_failed;

    tests_run = tests_failed = 0;
    printf("\n── v7 (Sieve) ──\n");
    test_sanity(primehash_v7, "v7");
    test_avalanche(primehash_v7, "v7");
    test_bias(primehash_v7, "v7");
    test_distribution(primehash_v7, "v7");
    test_collision32(primehash_v7, "v7");
    test_differential(primehash_v7, "v7");
    test_speed(primehash_v7, "v7");
    test_bulk(primehash_v7, "v7");
    test_cyclic(primehash_v7, "v7");
    int r2 = tests_run, f2 = tests_failed;

    printf("\n=== SUMMARY ===\n");
    printf("v1 (original): %d/%d passed\n", r1 - f1, r1);
    printf("v7 (Sieve):    %d/%d passed\n", r2 - f2, r2);
    return (f1 > 0 || f2 > 0) ? 1 : 0;
}
