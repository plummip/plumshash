/*
 * bench_hybrid.c — 3-way fair bench: PlumsHash vs PlumsHash-Hybrid vs RapidHash
 *
 * Same methodology as bench_compare.c (alternating order, per-hash warmup +
 * asm barrier, same buffer/iterations), extended to min-of-3 for DVFS noise
 * on mobile SoCs, and denser size coverage around the 256B crossover.
 *
 * Compile: clang -O3 -march=native -Wall -Wextra -o bench_hybrid bench_hybrid.c
 */
#define PLUMSHASH_IMPLEMENTATION
#include "plumshash.h"
#define PLUMSHASH_HYBRID_IMPLEMENTATION
#include "plumshash_hybrid.h"
#define RAPIDHASH_IMPLEMENTATION
#include "rapidhash.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

enum { H_PLUMS = 0, H_HYBRID = 1, H_RAPID = 2, H_COUNT = 3 };

static uint64_t hash_call(int which, const void *buf, size_t len, uint64_t seed) {
    switch (which) {
        case H_PLUMS:  return plumshash(buf, len, seed);
        case H_HYBRID: return plumshash_hybrid(buf, len, seed);
        default:       return rapidhash_withSeed(buf, len, seed);
    }
}

static double bench_one(int which, const void *buf, size_t len, int iters,
                        double *ns_out) {
    uint64_t acc = 0;
    acc += hash_call(which, buf, len, 0);
    __asm__ volatile("" : "+r"(acc));

    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    for (int i = 0; i < iters; i++)
        acc += hash_call(which, buf, len, acc ^ (uint64_t)i);
    clock_gettime(CLOCK_MONOTONIC, &t1);
    __asm__ volatile("" : "+r"(acc));

    double sec = (t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) * 1e-9;
    *ns_out = sec * 1e9 / iters;
    return ((double)len * iters / 1e9) / sec;
}

int main(void) {
    const int iterations = 2000000;
    uint8_t *buf = (uint8_t*)malloc(8192);
    if (!buf) return 1;
    memset(buf, 0xAB, 8192);

    struct { int len; const char *label; } sizes[] = {
        {0, "0B"}, {1, "1B"}, {3, "3B"}, {4, "4B"}, {8, "8B"},
        {12, "12B"}, {16, "16B"}, {17, "17B"}, {24, "24B"}, {32, "32B"},
        {47, "47B"}, {48, "48B"}, {64, "64B"}, {96, "96B"}, {127, "127B"},
        {128, "128B"}, {191, "191B"}, {255, "255B"}, {256, "256B"},
        {257, "257B"}, {384, "384B"}, {512, "512B"}, {768, "768B"},
        {1024, "1KB"}, {2048, "2KB"}, {4096, "4KB"}, {0, NULL}
    };

    printf("PlumsHash vs PlumsHash-Hybrid vs RapidHash (min-of-3, alternating)\n");
    printf("%-6s | %9s %7s | %9s %7s | %9s %7s | %6s %6s\n",
           "Size", "Plums", "ns/h", "Hybrid", "ns/h", "Rapid", "ns/h",
           "H/P", "H/R");
    printf("------ | ---------- ------- | ---------- ------- | ---------- ------- | ------ ------\n");

    double best[H_COUNT];
    for (int si = 0; sizes[si].label; si++) {
        int len   = sizes[si].len;
        int iters = (len < 64) ? iterations * 4 : iterations;
        int order[H_COUNT];
        /* rotate start hash per size to avoid systematic second-run bias */
        int start = si % H_COUNT;
        for (int k = 0; k < H_COUNT; k++) order[(start + k) % H_COUNT] = k;

        for (int k = 0; k < H_COUNT; k++) best[k] = 1e30;
        double ns[H_COUNT];

        for (int rep = 0; rep < 3; rep++) {
            for (int k = 0; k < H_COUNT; k++) {
                int w = order[k];
                double nsx;
                double gb = bench_one(w, buf, (size_t)len, iters, &nsx);
                if (gb < best[w]) { best[w] = gb; ns[w] = nsx; }
            }
        }

        double gb_p = best[H_PLUMS], gb_h = best[H_HYBRID], gb_r = best[H_RAPID];
        printf("%-6s | %8.2f %6.1f | %8.2f %6.1f | %8.2f %6.1f | %5.2fx %5.2fx\n",
               sizes[si].label, gb_p, ns[H_PLUMS], gb_h, ns[H_HYBRID],
               gb_r, ns[H_RAPID], gb_h / gb_p, gb_h / gb_r);
    }

    free(buf);
    return 0;
}
