/* test_comb16.c — exact SMHasher "Combination 16-bytes [0-1]" reproduction */
#define PLUMSHASH_IMPLEMENTATION
#include "plumshash.h"
#define RAPIDHASH_IMPLEMENTATION
#include "refs/rapidhash.h"
#define XXH_INLINE_ALL
#include "xxhash.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

static int cmp_u64(const void *a, const void *b) {
    uint64_t x = *(const uint64_t*)a, y = *(const uint64_t*)b;
    return x < y ? -1 : x > y ? 1 : 0;
}

/* 16-byte blocks {all-zero, first-byte-1}, sequences up to 22 blocks (352B) */
static void run(const char *name, uint64_t (*fn)(const void*, size_t, uint64_t)) {
    uint8_t key[384];
    static uint64_t hs[100000];
    printf("-- %s --\n", name);
    for (int n = 1; n <= 22; n++) {
        long keys_n = 1L << n;
        long sample = keys_n > 100000 ? 100000 : keys_n;
        long step = keys_n / sample;
        for (long i = 0; i < sample; i++) {
            long k = i * step;
            memset(key, 0, sizeof(key));
            for (int b = 0; b < n; b++)
                if ((k >> b) & 1) key[b * 16] = 1;   /* first byte of block */
            hs[i] = fn(key, (size_t)n * 16, 0);
        }
        qsort(hs, (size_t)sample, 8, cmp_u64);
        long distinct = 1, maxg = 1, cur = 1;
        for (long i = 1; i < sample; i++) {
            if (hs[i] == hs[i-1]) { cur++; if (cur > maxg) maxg = cur; }
            else { distinct++; cur = 1; }
        }
        printf("  n=%2d len=%3d: %6ld keys -> %6ld distinct, maxgroup=%ld\n",
               n, n * 16, sample, distinct, maxg);
    }
}

int main(void) {
    run("plumshash", plumshash);
    run("xxh3", XXH3_64bits_withSeed);
    run("rapidhash", rapidhash_withSeed);
    return 0;
}
