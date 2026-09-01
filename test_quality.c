/*
 * test_quality.c — SMHasher-grade quality checks for PlumsHash v1
 * Mirrors smhasher_plums.c checks/thresholds (audit report section 1).
 * Compile: clang -O2 -o test_quality test_quality.c -lm
 */
#define PLUMSHASH_IMPLEMENTATION
#include "plumshash.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

static inline int popcount(uint64_t x) {
    x -= (x >> 1) & 0x5555555555555555ULL;
    x  = (x & 0x3333333333333333ULL) + ((x >> 2) & 0x3333333333333333ULL);
    x  = (x + (x >> 4)) & 0x0F0F0F0F0F0F0F0FULL;
    return (int)((x * 0x0101010101010101ULL) >> 56);
}
static int pass = 0, fail = 0;
#define TEST(name, ok) do { if (ok) pass++; else { fail++; printf("  FAIL: %s\n", name); } } while(0)
#define REPORT() printf("\n%d passed, %d failed\n", pass, fail)

static uint64_t rng_state = 0x243F6A8885A308D3ULL;
static uint64_t rng(void) {
    rng_state ^= rng_state << 13; rng_state ^= rng_state >> 7;
    rng_state ^= rng_state << 17; return rng_state;
}

static void test_sanity(void) {
    printf("-- Sanity --\n");
    TEST("deterministic", plumshash("hello", 5, 42) == plumshash("hello", 5, 42));
    TEST("seed_diff", plumshash("hello", 5, 0) != plumshash("hello", 5, 1));
    TEST("key_diff", plumshash("hello", 5, 0) != plumshash("world", 5, 0));
    TEST("empty_seed0_nonzero", plumshash("", 0, 0) != 0);
    TEST("empty_key", plumshash("", 0, 42) != 0);
    TEST("null_zero_len", plumshash(NULL, 0, 42) != 0);
    uint8_t a[4] = {1,2,3,0}, b[3] = {1,2,3};
    TEST("len_matters", plumshash(a, 4, 0) != plumshash(b, 3, 0));
}

static void avalanche_at(size_t len, const char *label, double threshold) {
    uint8_t buf[1024];
    for (size_t i = 0; i < 1024; i++) buf[i] = (uint8_t)(i * 0x9D + 0x37);
    double worst = 100.0, sum = 0.0;
    int n = 0;
    int nbytes = (int)(len < 32 ? len : 32);
    for (int by = 0; by < nbytes; by++) {
        for (int bi = 0; bi < 8; bi++) {
            buf[by] ^= (uint8_t)(1u << bi);
            uint64_t h0 = plumshash(buf, len, 42);
            buf[by] ^= (uint8_t)(1u << bi);
            uint64_t h1 = plumshash(buf, len, 42);
            double p = popcount(h0 ^ h1) / 64.0 * 100.0;
            if (p < worst) worst = p;
            sum += p; n++;
        }
    }
    printf("  avalanche %s (%zuB): avg=%.1f%% worst=%.1f%%\n", label, len, sum / n, worst);
    TEST("avalanche", worst >= threshold);
}

static void test_avalanche(void) {
    printf("-- Avalanche --\n");
    avalanche_at(16, "tiny-mum", 25.0);
    avalanche_at(32, "serial-mum", 25.0);
    avalanche_at(64, "serial-mum", 25.0);
    avalanche_at(192, "mum-7lane", 25.0);
    avalanche_at(256, "rot-bulk", 25.0);
    avalanche_at(1024, "rot-bulk", 25.0);
}

static void test_differential(void) {
    printf("-- Differential --\n");
    uint8_t buf[64];
    for (int i = 0; i < 64; i++) buf[i] = (uint8_t)i;
    double sum = 0.0; int n = 512;
    for (int i = 0; i < n; i++)
        sum += popcount(plumshash(buf, 64, i) ^ plumshash(buf, 64, i + 1));
    double avg = sum / n;
    printf("  avg diff bits (seed+1): %.1f / 64\n", avg);
    TEST("differential_seed", avg > 25.0 && avg < 40.0);
    sum = 0.0;
    for (int i = 0; i < n; i++) {
        memset(buf, i & 0xFF, 64);
        uint64_t h0 = plumshash(buf, 64, 12345);
        memset(buf, (i + 1) & 0xFF, 64);
        uint64_t h1 = plumshash(buf, 64, 12345 + i);
        sum += popcount(h0 ^ h1);
    }
    avg = sum / n;
    printf("  avg diff bits (pattern): %.1f / 64\n", avg);
    TEST("differential_pattern", avg > 25.0);
}

static void test_chi2(void) {
    printf("-- chi-squared --\n");
    int bins[256] = {0};
    const int N = 256000;
    uint8_t key[8];
    for (int i = 0; i < N; i++) {
        uint64_t r = rng();
        memcpy(key, &r, 8);
        bins[plumshash(key, 8, i) & 0xFF]++;
    }
    double chi = 0.0;
    for (int i = 0; i < 256; i++) {
        double d = bins[i] - (double)N / 256.0;
        chi += d * d / ((double)N / 256.0);
    }
    printf("  chi2 = %.1f (threshold 300)\n", chi);
    TEST("chi2", chi < 300.0);
}

static void test_sparse(void) {
    printf("-- Sparse (audit methodology: 64B keys, 4 nonzero bytes, 65536 buckets) --\n");
    int bins[65536] = {0};
    uint8_t key[64];
    const int N = 100000;
    for (int i = 0; i < N; i++) {
        memset(key, 0, 64);
        for (int k = 0; k < 4; k++)
            key[rng() % 64] = (uint8_t)rng();
        bins[(plumshash(key, 64, 0) >> 48) & 0xFFFF]++;
    }
    long maxd = 0; double chi = 0.0;
    for (int i = 0; i < 65536; i++) {
        if (bins[i] > maxd) maxd = bins[i];
        double d = bins[i] - (double)N / 65536.0;
        chi += d * d / ((double)N / 65536.0);
    }
    printf("  sparse: maxdepth=%ld (random ~9), chi2=%.0f (baseline 65536)\n", maxd, chi);
    TEST("sparse_maxdepth", maxd <= 14);
    TEST("sparse_chi2", chi < 130000.0);   /* < 2x random baseline */
}

static void test_permutation(void) {
    printf("-- Permutation --\n");
    uint8_t key[16];
    for (int i = 0; i < 16; i++) key[i] = (uint8_t)(i + 1);
    uint64_t h0 = plumshash(key, 16, 0);
    int diff = 0;
    for (int p = 1; p < 32; p++) {
        uint8_t t = key[0];
        memmove(key, key + 1, 15);
        key[15] = t;
        if (plumshash(key, 16, 0) != h0) diff++;
    }
    printf("  permutations differ: %d/31\n", diff);
    TEST("permutation", diff >= 30);
}

static void test_appended_zeroes(void) {
    printf("-- AppendedZeroes --\n");
    uint8_t key[20];
    int diff = 0;
    for (int i = 0; i < 16; i++) {
        for (int j = 0; j <= i; j++) key[j] = (uint8_t)(j + 1);
        uint64_t a = plumshash(key, i + 1, 0);
        key[i + 1] = 0;
        uint64_t b = plumshash(key, i + 2, 0);
        if (a != b) diff++;
    }
    printf("  appended-zero differ: %d/16\n", diff);
    TEST("appended_zeroes", diff >= 15);
}

static void test_seed_independence(void) {
    printf("-- Seed independence --\n");
    uint8_t key[32];
    for (int i = 0; i < 32; i++) key[i] = (uint8_t)(i * 7);
    double sum = 0.0;
    for (int s = 0; s < 50; s++) {
        uint64_t a = plumshash(key, 32, s);
        uint64_t b = plumshash(key, 32, s + 1);
        sum += popcount(a ^ b);
    }
    double avg = sum / 50.0;
    printf("  seed+1 avg diff: %.1f / 64\n", avg);
    TEST("seed_independence", avg > 25.0 && avg < 40.0);
}

int main(void) {
    test_sanity();
    test_avalanche();
    test_differential();
    test_chi2();
    test_sparse();
    test_permutation();
    test_appended_zeroes();
    test_seed_independence();
    REPORT();
    return fail ? 1 : 0;
}
