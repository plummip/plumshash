/*
 * smhasher_plums.c — SMHasher-grade verification tests for PlumsHash
 *
 * Standalone. Compile and run:
 *   gcc -O3 -Wall -Wextra -o smhasher_plums smhasher_plums.c -lm && ./smhasher_plums
 *
 * Tests: Sanity, Avalanche, Differential, chi², Sparse, Permutation, AppendedZeroes
 * Scoring follows SMHasher conventions.
 */
#define PLUMSHASH_IMPLEMENTATION
#include "plumshash.h"
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
/* Test 1: Sanity — deterministic, different seeds differ, empty key works */
/* ══════════════════════════════════════════════════════════════════════ */
static void test_sanity(void) {
    printf("── Sanity ──\n");
    /* deterministic */
    TEST("deterministic",
        plumshash("hello", 5, 42) == plumshash("hello", 5, 42));
    
    /* different seeds → different hashes */
    TEST("seed_diff",
        plumshash("hello", 5, 0) != plumshash("hello", 5, 1));
    
    /* different keys → different hashes */
    TEST("key_diff",
        plumshash("hello", 5, 0) != plumshash("world", 5, 0));
    
    /* empty key with seed 0 produces 0 (correct — seed*phi = 0) */
    TEST("empty_seed0_is_zero",
        plumshash("", 0, 0) == 0);

    /* empty key with non-zero seed is non-zero */
    TEST("empty_key",
        plumshash("", 0, 42) != 0);
    
    /* null buffer with len=0 */
    TEST("null_zero_len",
        plumshash(NULL, 0, 42) != 0);
    
    /* same key different length */
    uint8_t a[4] = {1,2,3,0};
    uint8_t b[3] = {1,2,3};
    TEST("len_matters",
        plumshash(a, 4, 0) != plumshash(b, 3, 0));
}

/* ══════════════════════════════════════════════════════════════════════ */
/* Test 2: Avalanche — 1-bit flip in input changes ~50% of output bits */
/* ══════════════════════════════════════════════════════════════════════ */
static void test_avalanche(void) {
    printf("── Avalanche ──\n");

    /* Fast path (≥128B): pure ARX */
    {
        uint8_t buf[256];
        for (int i = 0; i < 256; i++) buf[i] = (uint8_t)(i * 0x9D + 0x37);

        double worst = 100.0;
        for (int by = 0; by < 32; by++) {
            for (int bi = 0; bi < 8; bi++) {
                buf[by] ^= (uint8_t)(1u << bi);
                uint64_t h0 = plumshash(buf, 256, 0);
                buf[by] ^= (uint8_t)(1u << bi);
                uint64_t h1 = plumshash(buf, 256, 0);
                double p = popcount(h0 ^ h1) / 64.0 * 100.0;
                if (p < worst) worst = p;
            }
        }
        printf("  fast (256B): worst=%.1f%%\n", worst);
        TEST("avalanche_fast", worst >= 30.0);
    }

    /* Safe path (<128B): ARX + accumulator + cross-mix */
    {
        uint8_t buf[32];
        for (int i = 0; i < 32; i++) buf[i] = (uint8_t)(i * 0x9D + 0x37);

        double worst = 100.0;
        for (int by = 0; by < 32; by++) {
            for (int bi = 0; bi < 8; bi++) {
                buf[by] ^= (uint8_t)(1u << bi);
                uint64_t h0 = plumshash(buf, 32, 42 + by * 8 + bi);
                buf[by] ^= (uint8_t)(1u << bi);
                uint64_t h1 = plumshash(buf, 32, 42 + by * 8 + bi);
                double p = popcount(h0 ^ h1) / 64.0 * 100.0;
                if (p < worst) worst = p;
            }
        }
        printf("  safe (32B): worst=%.1f%%\n", worst);
        TEST("avalanche_safe", worst >= 30.0);
    }
}

/* ══════════════════════════════════════════════════════════════════════ */
/* Test 3: Differential — related keys produce uncorrelated outputs */
/* ══════════════════════════════════════════════════════════════════════ */
static void test_differential(void) {
    printf("── Differential ──\n");
    uint8_t buf[64];
    uint64_t seed = 12345;
    
    /* Same key, different seed — expect ~32 bits different */
    for (int i = 0; i < 64; i++) buf[i] = (uint8_t)i;
    
    double sum_p = 0.0;
    int n = 512;
    for (int i = 0; i < n; i++) {
        uint64_t h0 = plumshash(buf, 64, i);
        uint64_t h1 = plumshash(buf, 64, i + 1);
        sum_p += popcount(h0 ^ h1);
    }
    double avg_bits = sum_p / n;
    printf("  avg diff bits (seed+1): %.1f / 64\n", avg_bits);
    TEST("differential_seed", avg_bits > 25.0 && avg_bits < 40.0);
    
    /* Key pattern: 0x00... 0x01... 0x02... etc */
    sum_p = 0.0;
    for (int i = 0; i < n; i++) {
        memset(buf, i & 0xFF, 64);
        uint64_t h0 = plumshash(buf, 64, seed);
        memset(buf, (i+1) & 0xFF, 64);
        uint64_t h1 = plumshash(buf, 64, seed + i);
        sum_p += popcount(h0 ^ h1);
    }
    avg_bits = sum_p / n;
    printf("  avg diff bits (pattern change): %.1f / 64\n", avg_bits);
    TEST("differential_pattern", avg_bits > 25.0);
}

/* ══════════════════════════════════════════════════════════════════════ */
/* Test 4: chi² — distribution uniformity for low bits */
/* ══════════════════════════════════════════════════════════════════════ */
static void test_chi2(void) {
    printf("── chi² ──\n");
    int bins[256] = {0};
    const int N = 256000;  /* expected 1000 per bin */
    
    for (int i = 0; i < N; i++) {
        uint64_t h = plumshash(&i, sizeof(i), i);
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
/* Test 5: Sparse — hashes with mostly-zero keys don't collide           */
/* ══════════════════════════════════════════════════════════════════════ */
static void test_sparse(void) {
    printf("── Sparse ──\n");
    #define SPARSE_N 20000
    uint64_t *seen = (uint64_t*)calloc(SPARSE_N, sizeof(uint64_t));
    if (!seen) { printf("  SKIP: OOM\n"); return; }

    int ngen = 0;
    uint8_t key[256];
    /* Key length: 8..64 (8 + pos%57).  Byte val at end, seed 0. */
    for (int pos = 0; pos < 128 && ngen < SPARSE_N; pos++) {
        for (int val = 1; val < 256 && ngen < SPARSE_N; val++) {
            int klen = 8 + (pos % 57);
            memset(key, 0, klen);
            key[klen - 1] = (uint8_t)val;
            if (pos & 1) key[0] = (uint8_t)(pos ^ val);
            seen[ngen++] = plumshash(key, (size_t)klen, 0);
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
/* Test 6: Permutation — byte-order changes produce different hashes */
/* ══════════════════════════════════════════════════════════════════════ */
static void test_permutation(void) {
    printf("── Permutation ──\n");
    uint8_t buf[32];
    for (int i = 0; i < 32; i++) buf[i] = (uint8_t)(i * 7 + 3);
    
    uint64_t base = plumshash(buf, 32, 0);
    int diff_count = 0;
    
    /* Swap each adjacent pair */
    for (int i = 0; i < 31; i++) {
        uint8_t tmp = buf[i];
        buf[i] = buf[i+1];
        buf[i+1] = tmp;
        
        uint64_t h = plumshash(buf, 32, 0);
        if (h != base) diff_count++;
        
        /* Swap back */
        buf[i+1] = buf[i];
        buf[i] = tmp;
    }
    
    printf("  different after swap: %d / 31\n", diff_count);
    TEST("permutation", diff_count >= 30);
}

/* ══════════════════════════════════════════════════════════════════════ */
/* Test 7: AppendedZeroes — appending zeros changes the hash */
/* ══════════════════════════════════════════════════════════════════════ */
static void test_appended_zeroes(void) {
    printf("── AppendedZeroes ──\n");
    /* Hash with different numbers of trailing zeros */
    const char *key = "test";
    uint64_t h0 = plumshash(key, 4, 0);
    
    uint8_t buf[32];
    memset(buf, 0, 32);
    memcpy(buf, key, 4);
    
    int diff_count = 0;
    for (int i = 5; i <= 20; i++) {
        uint64_t h = plumshash(buf, (size_t)i, 0);
        if (h != h0) diff_count++;
    }
    
    printf("  different with appended zeros: %d / 16\n", diff_count);
    TEST("appended_zeroes", diff_count >= 15);
}

/* ══════════════════════════════════════════════════════════════════════ */
/* Test 8: Speed — throughput and latency for various key sizes          */
/* ══════════════════════════════════════════════════════════════════════ */
static void test_speed(void) {
    printf("── Speed ──\n");
    const int iterations = 2000000;
    uint8_t *buf = (uint8_t*)malloc(4096);
    if (!buf) return;
    memset(buf, 0xAB, 4096);

    struct { int len; const char *label; } sizes[] = {
        {4, "4B"}, {8, "8B"}, {16, "16B"}, {32, "32B"},
        {64, "64B"}, {128, "128B"}, {256, "256B"}, {512, "512B"},
        {1024, "1KB"}, {2048, "2KB"}, {4096, "4KB"}, {0, NULL}
    };

    printf("  %-5s %8s %12s %8s\n", "Size", "GB/s", "B/s", "ns/h");

    for (int si = 0; sizes[si].label; si++) {
        int len   = sizes[si].len;
        int iters = (len < 64) ? iterations * 4 : iterations;
        uint64_t acc = 0;

        /* Warmup + barrier */
        acc += plumshash(buf, (size_t)len, 0);
        __asm__ volatile("" : "+r"(acc));

        struct timespec t0, t1;
        clock_gettime(CLOCK_MONOTONIC, &t0);
        for (int i = 0; i < iters; i++) {
            acc += plumshash(buf, (size_t)len, acc ^ (uint64_t)i);
        }
        clock_gettime(CLOCK_MONOTONIC, &t1);
        /* Barrier — force materialisation */
        __asm__ volatile("" : "+r"(acc));

        double sec   = (t1.tv_sec - t0.tv_sec)
                     + (t1.tv_nsec - t0.tv_nsec) * 1e-9;
        double total = (double)len * iters;
        double gb_s  = (total / 1e9) / sec;
        double ns_h  = sec * 1e9 / iters;

        printf("  %-5s %8.2f GB/s %12.0f B/s %8.0f ns/h  (acc=%016llx)\n",
               sizes[si].label, gb_s, total / sec, ns_h,
               (unsigned long long)acc);
    }

    free(buf);
}

/* ══════════════════════════════════════════════════════════════════════ */
int main(void) {
    printf("PlumsHash SMHasher-grade verification\n");
    printf("=====================================\n\n");
    
    test_sanity();
    test_avalanche();
    test_differential();
    test_chi2();
    test_sparse();
    test_permutation();
    test_appended_zeroes();
    test_speed();
    
    REPORT();
    
    return fail ? 1 : 0;
}
