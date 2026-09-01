/*
 * bench_compete.c — plumshash vs the field: wyhash, XXH3, t1ha2, rapidhash.
 * Two harnesses:
 *   LATENCY    serial seed dependency (hash-table model), fixed buffer
 *   THROUGHPUT independent seeds over a rotating chunk pool (ILP model)
 * Randomized (variant,size) order, min-of-3, adaptive iterations.
 * Compile: clang -O3 -march=native -o bench_compete bench_compete.c refs/t1ha/t1ha2.c -Irefs
 */
#define PLUMSHASH_IMPLEMENTATION
#include "plumshash.h"
#define RAPIDHASH_IMPLEMENTATION
#include "refs/rapidhash.h"
#include "refs/wyhash.h"
#define XXH_INLINE_ALL
#include "xxhash.h"
#include "t1ha.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

typedef uint64_t (*hashfn)(const void*, size_t, uint64_t);
static hashfn variants[5];
static const char *vnames[5] = { "plums", "rapid", "wyhash", "xxh3", "t1ha2" };

static uint64_t wy_wrap(const void *b, size_t l, uint64_t s) { return wyhash(b, l, s, _wyp); }
static uint64_t xx3_wrap(const void *b, size_t l, uint64_t s) { return XXH3_64bits_withSeed(b, l, s); }
static uint64_t t1_wrap(const void *b, size_t l, uint64_t s) { return t1ha2_atonce(b, l, s); }

static uint64_t rng_state = 0x9E3779B97F4A7C15ULL;
static uint64_t rng(void) {
    rng_state ^= rng_state << 13; rng_state ^= rng_state >> 7;
    rng_state ^= rng_state << 17; return rng_state;
}

/* latency: serial dependency, fixed buffer */
static double bench_lat(hashfn fn, const uint8_t *buf, size_t len, int iters, double *ns_out) {
    uint64_t acc = 0;
    acc += fn(buf, len, 0);
    __asm__ volatile("" : "+r"(acc));
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    for (int i = 0; i < iters; i++)
        acc += fn(buf, len, acc ^ (uint64_t)i);
    clock_gettime(CLOCK_MONOTONIC, &t1);
    __asm__ volatile("" : "+r"(acc));
    double sec = (t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) * 1e-9;
    *ns_out = sec * 1e9 / iters;
    return ((double)len * iters / 1e9) / sec;
}

/* throughput: independent seeds, rotating chunks of a pool */
static double bench_tput(hashfn fn, const uint8_t *pool, size_t poolsz, size_t len,
                         int iters, double *ns_out) {
    size_t nchunks = (len ? poolsz / len : 1);
    uint64_t acc = 0;
    acc += fn(pool, len, 0);
    __asm__ volatile("" : "+r"(acc));
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    for (int i = 0; i < iters; i++) {
        const uint8_t *p = pool + (size_t)(i % nchunks) * len;
        acc += fn(p, len, (uint64_t)i);
    }
    clock_gettime(CLOCK_MONOTONIC, &t1);
    __asm__ volatile("" : "+r"(acc));
    double sec = (t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) * 1e-9;
    *ns_out = sec * 1e9 / iters;
    return ((double)len * iters / 1e9) / sec;
}

int main(void) {
    variants[0] = plumshash; variants[1] = rapidhash_withSeed;
    variants[2] = wy_wrap; variants[3] = xx3_wrap; variants[4] = t1_wrap;

    const size_t POOL = 4u << 20;   /* 4MB chunk pool */
    uint8_t *pool = (uint8_t*)malloc(POOL + 64);
    uint8_t *buf = (uint8_t*)malloc(65536);
    if (!pool || !buf) return 1;
    memset(pool, 0xAB, POOL + 64);
    memset(buf, 0xAB, 65536);

    struct { int len; const char *label; } sizes[] = {
        {0,"0B"},{1,"1B"},{3,"3B"},{4,"4B"},{8,"8B"},{12,"12B"},{16,"16B"},
        {17,"17B"},{24,"24B"},{32,"32B"},{47,"47B"},{48,"48B"},{64,"64B"},
        {96,"96B"},{112,"112B"},{128,"128B"},{160,"160B"},{192,"192B"},
        {224,"224B"},{255,"255B"},{256,"256B"},{320,"320B"},{384,"384B"},
        {512,"512B"},{768,"768B"},{1024,"1KB"},{2048,"2KB"},{4096,"4KB"},
        {8192,"8KB"},{16384,"16KB"},{0,NULL}
    };
    int nsizes = 0; while (sizes[nsizes].label) nsizes++;

    enum { LAT, TP, NHARN };
    const char *hnames[NHARN] = { "LATENCY", "THROUGHPUT" };

    for (int h = 0; h < NHARN; h++) {
        /* shuffled (size,variant) pairs, 3 reps */
        struct pair { int si, v; } pairs[31 * 5 * 3];
        int np = 0;
        for (int r = 0; r < 3; r++)
            for (int si = 0; si < nsizes; si++)
                for (int v = 0; v < 5; v++) { pairs[np].si = si; pairs[np].v = v; np++; }
        for (int i = np - 1; i > 0; i--) {
            int j = (int)(rng() % (uint64_t)(i + 1));
            struct pair t = pairs[i]; pairs[i] = pairs[j]; pairs[j] = t;
        }
        double best[31][5];
        for (int si = 0; si < nsizes; si++) for (int v = 0; v < 5; v++) best[si][v] = 1e30;
        double nsbest[31][5];

        for (int k = 0; k < np; k++) {
            int si = pairs[k].si, v = pairs[k].v;
            int len = sizes[si].len;
            int iters;
            if (h == LAT) {
                iters = len < 64 ? 4000000 : len < 1024 ? 1000000 : len < 8192 ? 300000 : 150000;
            } else {
                iters = len == 0 ? 2000000 : (int)((1u << 30) / (size_t)len);
                if (iters < 2000) iters = 2000;
                if (iters > 8000000) iters = 8000000;
            }
            double nsx, gb;
            if (h == LAT) gb = bench_lat(variants[v], buf, (size_t)len, iters, &nsx);
            else          gb = bench_tput(variants[v], pool, POOL, (size_t)len, iters, &nsx);
            if (gb < best[si][v]) { best[si][v] = gb; nsbest[si][v] = nsx; }
            if ((k & 7) == 7) usleep(12000);
        }

        printf("=== %s (min-of-3, GB/s; ns/op in parens) ===\n", hnames[h]);
        printf("%-6s |", "Size");
        for (int v = 0; v < 5; v++) printf(" %16s", vnames[v]);
        printf("\n------ |");
        for (int v = 0; v < 5; v++) printf(" ----------------");
        printf("\n");
        for (int si = 0; si < nsizes; si++) {
            printf("%-6s |", sizes[si].label);
            for (int v = 0; v < 5; v++) {
                double gb = best[si][v];
                if (gb > 1e29) { printf(" %16s", "-"); continue; }
                if (sizes[si].len == 0) printf(" %12.1fns  ", nsbest[si][v]);
                else printf(" %9.2f(%5.1f)", gb, nsbest[si][v]);
            }
            printf("\n");
        }
        printf("\n");
    }
    free(pool); free(buf);
    return 0;
}
