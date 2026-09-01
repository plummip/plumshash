/*
 * test_speed.c — bandwidth benchmark for PlumsHash
 *
 * Measures throughput (GB/s, B/s) and latency (ns/hash) for key
 * sizes 4B through 4KB.  Uses clock_gettime() for nanosecond
 * wall‑clock timing.  Inline asm barriers prevent DCE.
 *
 * Compile:  gcc -O3 -Wall -Wextra -o test_speed test_speed.c
 * Run:      ./test_speed
 */
#define PLUMSHASH_IMPLEMENTATION
#include "plumshash.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

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

    printf("%-6s %10s %14s %10s\n", "Size", "GB/s", "B/s", "ns/hash");
    printf("------ ---------- -------------- ----------\n");

    for (int si = 0; sizes[si].label; si++) {
        int len   = sizes[si].len;
        int iters = (len < 64) ? iterations * 4 : iterations;
        uint64_t acc = 0;

        /* Warmup — one call to prime caches, then asm barrier */
        acc += plumshash(buf, (size_t)len, 0);
        __asm__ volatile("" : "+r"(acc));

        struct timespec t0, t1;
        clock_gettime(CLOCK_MONOTONIC, &t0);
        for (int i = 0; i < iters; i++) {
            acc += plumshash(buf, (size_t)len, acc ^ (uint64_t)i);
        }
        clock_gettime(CLOCK_MONOTONIC, &t1);
        /* Barrier — force compiler to materialise acc */
        __asm__ volatile("" : "+r"(acc));

        double sec   = (t1.tv_sec - t0.tv_sec)
                     + (t1.tv_nsec - t0.tv_nsec) * 1e-9;
        double total = (double)len * iters;
        double gb_s  = (total / 1e9) / sec;
        double ns_h  = sec * 1e9 / iters;

        printf("%-6s %8.2f GB/s %12.0f B/s %8.1f ns/h  (acc=%016llx)\n",
               sizes[si].label, gb_s, total / sec, ns_h,
               (unsigned long long)acc);
    }

    free(buf);
    return 0;
}
