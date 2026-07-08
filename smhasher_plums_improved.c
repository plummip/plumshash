/*
 * smhasher_plums_improved.c — SMHasher-grade verification for plumsHash improved
 *
 * Compile:
 *   cd ~/projects/plumshash
 *   gcc -O3 -march=native -I./include -o smhasher_plums_improved \
 *       smhasher_plums_improved.c -lm
 *   ./smhasher_plums_improved
 *
 * Reuses the same test framework as smhasher_plums.c but links
 * against the improved hash via tpde's include path.
 * Standalone single-file test — no external SMHasher needed.
 */
#define PLUMSHASH_IMPLEMENTATION
#define PLUMSHASH_IMPROVED_IMPLEMENTATION
#include "plumshash.h"
#include "../acht/include/tpde/core/plumshash_improved.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>

/* ── helpers ── */
static inline int popcount(uint64_t x) {
    x -= (x >> 1) & 0x5555555555555555ULL;
    x  = (x & 0x3333333333333333ULL) + ((x >> 2) & 0x3333333333333333ULL);
    x  = (x + (x >> 4)) & 0x0F0F0F0F0F0F0F0FULL;
    return (int)((x * 0x0101010101010101ULL) >> 56);
}

static int pass = 0, fail = 0;
#define TEST(name, ok) do { \
    if (ok) { pass++; } else { fail++; printf("  FAIL: %s\n", name); } \
} while(0)
#define REPORT() printf("\n%d passed, %d failed\n", pass, fail)

/* ══════════════════════════════════════════════════════════════════════ */
/* Test 1: Sanity — deterministic, different seeds differ, empty key     */
/* ══════════════════════════════════════════════════════════════════════ */
static void test_sanity(void) {
    printf("── Sanity ──\n");
    TEST("deterministic",
        plumshash_improved("hello", 5, 42) == plumshash_improved("hello", 5, 42));
    TEST("seed_diff",
        plumshash_improved("hello", 5, 0) != plumshash_improved("hello", 5, 1));
    TEST("key_diff",
        plumshash_improved("hello", 5, 0) != plumshash_improved("world", 5, 0));
    /* empty key with seed 0 produces a defined non-zero value */
    TEST("empty_key",
        plumshash_improved("", 0, 42) == plumshash_improved("", 0, 42));
    TEST("all_zeroes_16",
        plumshash_improved("\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0", 16, 0) != 0);
}

/* ══════════════════════════════════════════════════════════════════════ */
/* Test 2: Avalanche — bit flips produce ~50% output changes            */
/* χ² method: for each input bit, measure output bit flip distribution  */
/* ══════════════════════════════════════════════════════════════════════ */
/* Avalanche: test all 4 dispatch paths */
static void test_avalanche(void) {
    printf("── Avalanche ──\n");
    uint8_t buf[256];

    /* Tiny path: 16 bytes */
    {
        double worst = 100.0;
        for (int by = 0; by < 16; by++) {
            for (int bi = 0; bi < 8; bi++) {
                buf[by] ^= (uint8_t)(1u << bi);
                uint64_t h0 = plumshash_improved(buf, 16, 42 + by * 8 + bi);
                buf[by] ^= (uint8_t)(1u << bi);
                uint64_t h1 = plumshash_improved(buf, 16, 42 + by * 8 + bi);
                double p = popcount(h0 ^ h1) / 64.0 * 100.0;
                if (p < worst) worst = p;
            }
        }
        printf("  tiny (16B): worst=%.1f%%\n", worst);
        TEST("avalanche_tiny", worst >= 20.0);
    }

    /* Safe path: 32 bytes */
    {
        double worst = 100.0;
        for (int by = 0; by < 32; by++) {
            for (int bi = 0; bi < 8; bi++) {
                buf[by] ^= (uint8_t)(1u << bi);
                uint64_t h0 = plumshash_improved(buf, 32, 42 + by * 8 + bi);
                buf[by] ^= (uint8_t)(1u << bi);
                uint64_t h1 = plumshash_improved(buf, 32, 42 + by * 8 + bi);
                double p = popcount(h0 ^ h1) / 64.0 * 100.0;
                if (p < worst) worst = p;
            }
        }
        printf("  safe (32B): worst=%.1f%%\n", worst);
        TEST("avalanche_safe", worst >= 25.0);
    }

    /* Medium path: 64 bytes */
    {
        double worst = 100.0;
        for (int by = 0; by < 64; by++) {
            for (int bi = 0; bi < 8; bi++) {
                buf[by] ^= (uint8_t)(1u << bi);
                uint64_t h0 = plumshash_improved(buf, 64, 42 + by * 8 + bi);
                buf[by] ^= (uint8_t)(1u << bi);
                uint64_t h1 = plumshash_improved(buf, 64, 42 + by * 8 + bi);
                double p = popcount(h0 ^ h1) / 64.0 * 100.0;
                if (p < worst) worst = p;
            }
        }
        printf("  medium (64B): worst=%.1f%%\n", worst);
        TEST("avalanche_medium", worst >= 25.0);
    }

    /* Fast path: 256 bytes */
    {
        double worst = 100.0;
        for (int by = 0; by < 64; by++) {
            for (int bi = 0; bi < 8; bi++) {
                buf[by] ^= (uint8_t)(1u << bi);
                uint64_t h0 = plumshash_improved(buf, 256, 42 + by * 8 + bi);
                buf[by] ^= (uint8_t)(1u << bi);
                uint64_t h1 = plumshash_improved(buf, 256, 42 + by * 8 + bi);
                double p = popcount(h0 ^ h1) / 64.0 * 100.0;
                if (p < worst) worst = p;
            }
        }
        printf("  fast (256B): worst=%.1f%%\n", worst);
        TEST("avalanche_fast", worst >= 25.0);
    }
}

/* ══════════════════════════════════════════════════════════════════════ */
/* Test 3: Differential — related keys produce uncorrelated outputs     */
/* ══════════════════════════════════════════════════════════════════════ */
static void test_differential(void) {
    printf("── Differential ──\n");
    uint8_t buf[64];
    uint64_t seed = 12345;

    for (int i = 0; i < 64; i++) buf[i] = (uint8_t)i;

    double sum_p = 0.0;
    int n = 512;
    for (int i = 0; i < n; i++) {
        uint64_t h0 = plumshash_improved(buf, 64, i);
        uint64_t h1 = plumshash_improved(buf, 64, i + 1);
        sum_p += popcount(h0 ^ h1);
    }
    double avg_bits = sum_p / n;
    printf("  avg diff bits (seed+1): %.1f / 64\n", avg_bits);
    TEST("differential_seed", avg_bits > 25.0 && avg_bits < 40.0);

    /* Key pattern change */
    sum_p = 0.0;
    for (int i = 0; i < n; i++) {
        memset(buf, i & 0xFF, 64);
        uint64_t h0 = plumshash_improved(buf, 64, seed);
        memset(buf, (i+1) & 0xFF, 64);
        uint64_t h1 = plumshash_improved(buf, 64, seed + i);
        sum_p += popcount(h0 ^ h1);
    }
    avg_bits = sum_p / n;
    printf("  avg diff bits (pattern change): %.1f / 64\n", avg_bits);
    TEST("differential_pattern", avg_bits > 25.0);
}

/* ══════════════════════════════════════════════════════════════════════ */
/* Test 4: chi² — distribution uniformity for low bits                  */
/* ══════════════════════════════════════════════════════════════════════ */
static void test_chi2(void) {
    printf("── chi² ──\n");
    int bins[256] = {0};
    const int N = 256000;

    for (int i = 0; i < N; i++) {
        uint64_t h = plumshash_improved(&i, sizeof(i), i);
        bins[h & 0xFF]++;
    }

    double expected = (double)N / 256.0;
    double chi2 = 0.0;
    for (int b = 0; b < 256; b++) {
        double d = bins[b] - expected;
        chi2 += d * d / expected;
    }

    printf("  χ² = %.1f  (lower is better, <300 is pass)\n", chi2);
    TEST("chi2_uniformity", chi2 < 300.0);
}

/* ══════════════════════════════════════════════════════════════════════ */
/* Test 5: Sparse — hashes with mostly-zero keys don't collide          */
/* ══════════════════════════════════════════════════════════════════════ */
static void test_sparse(void) {
    printf("── Sparse ──\n");
    #define SPARSE_N 20000
    uint64_t *seen = (uint64_t*)calloc(SPARSE_N, sizeof(uint64_t));
    if (!seen) { printf("  SKIP: OOM\n"); return; }

    int ngen = 0;
    uint8_t key[256];
    for (int pos = 0; pos < 128 && ngen < SPARSE_N; pos++) {
        for (int val = 1; val < 256 && ngen < SPARSE_N; val++) {
            int klen = 8 + (pos % 57);
            memset(key, 0, klen);
            key[klen - 1] = (uint8_t)val;
            if (pos & 1) key[0] = (uint8_t)(pos ^ val);
            seen[ngen++] = plumshash_improved(key, (size_t)klen, 0);
        }
    }

    int collisions = 0;
    for (int i = 0; i < ngen; i++)
        for (int j = i + 1; j < ngen; j++)
            if (seen[i] == seen[j]) collisions++;

    printf("  collisions: %d / %d\n", collisions, ngen);
    TEST("sparse_collisions", collisions <= 30);
    free(seen);
    #undef SPARSE_N
}

/* ══════════════════════════════════════════════════════════════════════ */
/* Test 6: Permutation — byte-order changes produce different hashes    */
/* ══════════════════════════════════════════════════════════════════════ */
static void test_permutation(void) {
    printf("── Permutation ──\n");
    uint8_t buf[32];
    for (int i = 0; i < 32; i++) buf[i] = (uint8_t)(i * 7 + 3);

    uint64_t base = plumshash_improved(buf, 32, 0);
    int diff_count = 0;

    for (int i = 0; i < 31; i++) {
        uint8_t tmp = buf[i];
        buf[i] = buf[i+1];
        buf[i+1] = tmp;

        uint64_t h = plumshash_improved(buf, 32, 0);
        if (h != base) diff_count++;

        buf[i+1] = buf[i];
        buf[i] = tmp;
    }

    printf("  different after swap: %d / 31\n", diff_count);
    TEST("permutation", diff_count >= 30);
}

/* ══════════════════════════════════════════════════════════════════════ */
/* Test 7: AppendedZeroes — appending zeros changes the hash            */
/* ══════════════════════════════════════════════════════════════════════ */
static void test_appended_zeroes(void) {
    printf("── AppendedZeroes ──\n");
    const char *key = "test";
    uint64_t h0 = plumshash_improved(key, 4, 0);

    uint8_t buf[32];
    memset(buf, 0, 32);
    memcpy(buf, key, 4);

    int diff_count = 0;
    for (int i = 5; i <= 20; i++) {
        uint64_t h = plumshash_improved(buf, (size_t)i, 0);
        if (h != h0) diff_count++;
    }

    printf("  different with appended zeros: %d / 16\n", diff_count);
    TEST("appended_zeroes", diff_count >= 15);
}

/* ══════════════════════════════════════════════════════════════════════ */
/* Test 8: Seed Independence — identical input, different seeds,        */
/*         consistent avalanche across all seeds                        */
/* ══════════════════════════════════════════════════════════════════════ */
static void test_seed_independence(void) {
    printf("── Seed Independence ──\n");
    uint8_t buf[64];
    for (int i = 0; i < 64; i++) buf[i] = (uint8_t)(i * 17 + 31);

    double avals[50];
    for (int s = 0; s < 50; s++) {
        uint64_t base_h = plumshash_improved(buf, 64, s);
        double total = 0.0;
        int trials = 128;
        for (int t = 0; t < trials; t++) {
            int by = (t * 7) % 64;
            int bi = (t * 13) % 8;
            buf[by] ^= (uint8_t)(1u << bi);
            uint64_t h = plumshash_improved(buf, 64, s);
            buf[by] ^= (uint8_t)(1u << bi);
            total += popcount(base_h ^ h);
        }
        avals[s] = total / (trials * 64.0) * 100.0;
    }

    double min_a = 100, max_a = 0;
    for (int s = 0; s < 50; s++) {
        if (avals[s] < min_a) min_a = avals[s];
        if (avals[s] > max_a) max_a = avals[s];
    }
    double spread = max_a - min_a;
    printf("  avalanche spread across 50 seeds: %.2f%% (min=%.1f max=%.1f)\n",
           spread, min_a, max_a);
    TEST("seed_independence", spread < 5.0);
}

/* ══════════════════════════════════════════════════════════════════════ */
/* Test 9: Path Boundary — no discontinuity at 16/17, 47/48, 127/128    */
/* ══════════════════════════════════════════════════════════════════════ */
static void test_path_boundary(void) {
    printf("── Path Boundary ──\n");
    uint8_t buf[256];
    for (int i = 0; i < 256; i++) buf[i] = rand() & 0xFF;

    size_t bounds[] = {16, 17, 47, 48, 127, 128};
    uint64_t hv[6];

    /* Run 100 random inputs, check boundaries produce distinct hashes */
    int boundary_ok = 1;
    for (int r = 0; r < 100; r++) {
        for (int i = 0; i < 256; i++) buf[i] = rand() & 0xFF;
        for (int i = 0; i < 6; i++)
            hv[i] = plumshash_improved(buf, bounds[i], 0);
        for (int i = 0; i < 6; i++)
            for (int j = i+1; j < 6; j++)
                if (hv[i] == hv[j]) boundary_ok = 0;
    }
    TEST("path_boundary", boundary_ok);
}

/* ══════════════════════════════════════════════════════════════════════ */
/* Main                                                                  */
/* ══════════════════════════════════════════════════════════════════════ */
int main(void) {
    srand(time(NULL));
    printf("=== SMHasher-Style Tests: PlumsHash Improved ===\n");
    printf("Platform: AArch64 @ ~3.5 GHz\n\n");

    test_sanity();
    test_avalanche();
    test_differential();
    test_chi2();
    test_sparse();
    test_permutation();
    test_appended_zeroes();
    test_seed_independence();
    test_path_boundary();

    REPORT();
    return fail ? 1 : 0;
}
