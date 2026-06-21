/*
 * wyhash_vs_primehash.c — Canonical wyhash v4.3 vs PrimeHash Quad
 * ===============================================================
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>
#include <time.h>

/* ═══════════════════════════════════════════════════════════════════
 * wyhash v4.3 — canonical implementation (public domain)
 * ═══════════════════════════════════════════════════════════════════ */

static inline void _wymum(uint64_t *A, uint64_t *B) {
#ifdef __SIZEOF_INT128__
    __uint128_t r = *A; r *= *B;
    *A = (uint64_t)r; *B = (uint64_t)(r >> 64);
#else
    uint64_t ha = *A >> 32, hb = *B >> 32, la = (uint32_t)*A, lb = (uint32_t)*B, hi, lo;
    uint64_t rh = ha * hb, rm0 = ha * lb, rm1 = hb * la, rl = la * lb, t = rl + (rm0 << 32), c = t < rl;
    lo = t + (rm1 << 32); c += lo < t; hi = rh + (rm0 >> 32) + (rm1 >> 32) + c;
    *A = lo; *B = hi;
#endif
}

static inline uint64_t _wymix(uint64_t A, uint64_t B) { _wymum(&A, &B); return A ^ B; }
static inline uint64_t _wyr8(const uint8_t *p) { uint64_t v; memcpy(&v, p, 8); return v; }
static inline uint64_t _wyr4(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }
static inline uint64_t _wyr3(const uint8_t *p, size_t k) { return (((uint64_t)p[0]) << 16) | (((uint64_t)p[k >> 1]) << 8) | p[k - 1]; }

static const uint64_t _wyp[4] = {
    0x2d358dccaa6c78a5ULL, 0x8bb84b93962eacc9ULL,
    0x4b33a62ed433d4a3ULL, 0x4d5a2da51de1aa47ULL
};

static inline uint64_t wyhash(const void *key, size_t len, uint64_t seed, const uint64_t *secret) {
    const uint8_t *p = (const uint8_t *)key;
    seed ^= _wymix(seed ^ secret[0], secret[1]);
    uint64_t a, b;

    if (len <= 16) {
        if (len >= 4) {
            a = (_wyr4(p) << 32) | _wyr4(p + ((len >> 3) << 2));
            b = (_wyr4(p + len - 4) << 32) | _wyr4(p + len - 4 - ((len >> 3) << 2));
        } else if (len > 0) {
            a = _wyr3(p, len); b = 0;
        } else {
            a = b = 0;
        }
    } else {
        size_t i = len;
        if (i >= 48) {
            uint64_t see1 = seed, see2 = seed;
            do {
                seed = _wymix(_wyr8(p) ^ secret[1], _wyr8(p + 8) ^ seed);
                see1 = _wymix(_wyr8(p + 16) ^ secret[2], _wyr8(p + 24) ^ see1);
                see2 = _wymix(_wyr8(p + 32) ^ secret[3], _wyr8(p + 40) ^ see2);
                p += 48; i -= 48;
            } while (i >= 48);
            seed ^= see1 ^ see2;
        }
        while (i > 16) { seed = _wymix(_wyr8(p) ^ secret[1], _wyr8(p + 8) ^ seed); i -= 16; p += 16; }
        a = _wyr8(p + i - 16); b = _wyr8(p + i - 8);
    }
    a ^= secret[1]; b ^= seed; _wymum(&a, &b);
    return _wymix(a ^ secret[0] ^ len, b ^ secret[1]);
}

/* wrapper to match our test signature */
uint64_t wyhash64_test(const void *key, size_t len, uint64_t seed) {
    return wyhash(key, len, seed, _wyp);
}

/* ═══════════════════════════════════════════════════════════════════
 * PrimeHash Quad
 * ═══════════════════════════════════════════════════════════════════ */
static inline uint64_t read64(const void *p) { uint64_t v; memcpy(&v, p, 8); return v; }
static inline uint64_t rotl64(uint64_t x, int k) { return (x << k) | (x >> (64 - k)); }
#define PHI64 0x9E3779B97F4A7C15ULL
#define PM1  0x85EBCA77C2B2AE3DULL
#define PM2  0xBF58476D1CE4E5B9ULL
#define PM4  0x94D049BB133111EBULL

uint64_t primehash_quad(const void *buf, size_t len, uint64_t seed) {
    const uint8_t *p = (const uint8_t *)buf;
    const uint8_t *end = p + len;
    uint64_t base = seed ^ (len * PHI64);
    uint64_t h1 = base * PHI64, h2 = base * PM1, h3 = base * PM2, h4 = base * PM4;
    while (p + 32 <= end) {
        h1 ^= rotl64(read64(p), 23); h1 *= PHI64; p += 8;
        h2 ^= rotl64(read64(p), 47); h2 *= PHI64; p += 8;
        h3 ^= rotl64(read64(p), 13); h3 *= PHI64; p += 8;
        h4 ^= rotl64(read64(p), 37); h4 *= PHI64; p += 8;
    }
    while (p + 8 <= end) { h1 ^= rotl64(read64(p), 23); h1 *= PHI64; p += 8; }
    uint64_t tail = 0;
    switch (end - p) {
        case 7: tail ^= (uint64_t)p[6] << 48; case 6: tail ^= (uint64_t)p[5] << 40;
        case 5: tail ^= (uint64_t)p[4] << 32; case 4: tail ^= (uint64_t)p[3] << 24;
        case 3: tail ^= (uint64_t)p[2] << 16; case 2: tail ^= (uint64_t)p[1] << 8;
        case 1: tail ^= (uint64_t)p[0]; h1 ^= rotl64(tail, 23); h1 *= PHI64;
    }
    h1 ^= h2; h3 ^= h4; h1 ^= h3;
    h1 ^= h1 >> 29; h1 *= PM1; h1 ^= h1 >> 31; h1 *= PM2;
    h1 ^= h1 >> 37; h1 *= PHI64; h1 ^= h1 >> 41;
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

/* ── tests ── */

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

    TEST("sanity: length differs");
    if (h("test", 4, 0) == h("test\0", 5, 0)) FAIL("%s length extension", tag);
    PASS();
}

int main(void) {
    printf("=== wyhash64 v4.3 vs PrimeHash Quad ===\n\n");

    tests_run = tests_failed = 0;
    printf("── wyhash64 ──\n");
    test_sanity(wyhash64_test, "wyhash64");
    test_avalanche(wyhash64_test, "wyhash64");
    test_bias(wyhash64_test, "wyhash64");
    test_distribution(wyhash64_test, "wyhash64");
    test_collision32(wyhash64_test, "wyhash64");
    test_differential(wyhash64_test, "wyhash64");
    test_speed(wyhash64_test, "wyhash64");
    test_bulk(wyhash64_test, "wyhash64");
    test_cyclic(wyhash64_test, "wyhash64");
    int wr = tests_run, wf = tests_failed;

    tests_run = tests_failed = 0;
    printf("\n── PrimeHash Quad ──\n");
    test_sanity(primehash_quad, "PrimeHash");
    test_avalanche(primehash_quad, "PrimeHash");
    test_bias(primehash_quad, "PrimeHash");
    test_distribution(primehash_quad, "PrimeHash");
    test_collision32(primehash_quad, "PrimeHash");
    test_differential(primehash_quad, "PrimeHash");
    test_speed(primehash_quad, "PrimeHash");
    test_bulk(primehash_quad, "PrimeHash");
    test_cyclic(primehash_quad, "PrimeHash");
    int pr = tests_run, pf = tests_failed;

    printf("\n=== SUMMARY ===\n");
    printf("wyhash64 v4.3:  %d/%d passed\n", wr - wf, wr);
    printf("PrimeHash Quad: %d/%d passed\n", pr - pf, pr);
    return (wf > 0 || pf > 0) ? 1 : 0;
}
