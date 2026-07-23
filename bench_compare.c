/*
 * bench_compare.c — Fair side-by-side: PlumsHash vs RapidHash
 *
 * Alternates call order per size to avoid second-run cache advantage.
 * Each hash gets independent warmup + asm barrier before timing.
 * Identical methodology for both — same buffer, iterations, seeds.
 *
 * Compile:  gcc -O3 -march=armv8-a -Wall -Wextra -o bench_compare bench_compare.c -I. -lm
 */
#define PLUMSHASH_IMPLEMENTATION
#include "plumshash.h"
#define RAPIDHASH_IMPLEMENTATION
#include "rapidhash.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static double bench_hash(int is_plums, const void *buf, size_t len, int iters,
                          double *ns_out) {
    uint64_t acc = 0;
    acc += (is_plums ? plumshash(buf, len, 0) : rapidhash_withSeed(buf, len, 0));
    __asm__ volatile("" : "+r"(acc));

    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    for (int i = 0; i < iters; i++) {
        if (is_plums)
            acc += plumshash(buf, len, acc ^ (uint64_t)i);
        else
            acc += rapidhash_withSeed(buf, len, acc ^ (uint64_t)i);
    }
    clock_gettime(CLOCK_MONOTONIC, &t1);
    __asm__ volatile("" : "+r"(acc));

    double sec = (t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) * 1e-9;
    *ns_out = sec * 1e9 / iters;
    return ((double)len * iters / 1e9) / sec;
}

int main(void) {
    const int iterations = 2000000;
    uint8_t *buf = (uint8_t*)malloc(4096);
    if (!buf) return 1;
    memset(buf, 0xAB, 4096);

    struct { int len; const char *label; } sizes[] = {
        {4, "4B"}, {8, "8B"}, {16, "16B"}, {32, "32B"},
        {64, "64B"}, {128, "128B"}, {256, "256B"}, {512, "512B"},
        {1024, "1KB"}, {2048, "2KB"}, {4096, "4KB"}, {0, NULL}
    };

    printf("PlumsHash vs RapidHash — fair comparison (alternating order)\n");
    printf("%-6s | %8s %7s | %8s %7s | %6s\n",
           "Size", "Plums", "ns/h", "Rapid", "ns/h", "Ratio");
    printf("------ | -------- ------- | -------- ------- | ------\n");

    for (int si = 0; sizes[si].label; si++) {
        int len   = sizes[si].len;
        int iters = (len < 64) ? iterations * 4 : iterations;
        int flip  = si & 1;

        double gb_p, gb_r, ns_p, ns_r;

        if (flip) {
            gb_r = bench_hash(0, buf, (size_t)len, iters, &ns_r);
            gb_p = bench_hash(1, buf, (size_t)len, iters, &ns_p);
        } else {
            gb_p = bench_hash(1, buf, (size_t)len, iters, &ns_p);
            gb_r = bench_hash(0, buf, (size_t)len, iters, &ns_r);
        }

        double ratio = gb_p / gb_r;
        const char *mark = ratio >= 1.0 ? "WIN" : "";

        printf("%-6s | %8.2f %6.0f | %8.2f %6.0f | %5.2fx %s\n",
               sizes[si].label, gb_p, ns_p, gb_r, ns_r, ratio, mark);
    }

    free(buf);
    return 0;
}
