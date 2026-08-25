/*
 * edgecheck_hybrid.c — correctness/edge sanity for plumshash_hybrid:
 *   - determinism, seed sensitivity, no collisions across small lengths
 *   - avalanche sanity at 16B / 256B (per-bit flips, first 8 bytes)
 *   - hybrid vs original plumshash output difference at >=256B (documents
 *     that bulk path here is NOT byte-identical to repo plums_fast)
 *
 * Compile: clang -O1 -g -fsanitize=address,undefined -o edgecheck_hybrid edgecheck_hybrid.c
 */
#define PLUMSHASH_IMPLEMENTATION
#include "plumshash.h"
#define PLUMSHASH_HYBRID_IMPLEMENTATION
#include "plumshash_hybrid.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint64_t rng_state = 0x123456789ABCDEF0ULL;
static uint64_t rng(void) {
    rng_state ^= rng_state << 13; rng_state ^= rng_state >> 7;
    rng_state ^= rng_state << 17; return rng_state;
}

static int bitcount64(uint64_t x) {
    int n = 0; while (x) { x &= x - 1; n++; } return n;
}

int main(void) {
    int fails = 0;
    uint8_t *buf = (uint8_t*)malloc(8192 + 16);
    if (!buf) return 1;
    for (int i = 0; i < 8192 + 16; i++) buf[i] = (uint8_t)rng();

    /* 1) determinism + distinctness across lengths 0..4096, 3 seeds */
    int distinct = 0, dup = 0;
    for (size_t len = 0; len <= 4096; len++) {
        uint64_t seeds[3] = {0, 1, 0xDEADBEEFCAFEF00DULL};
        uint64_t h0 = plumshash_hybrid(buf, len, seeds[0]);
        uint64_t h1 = plumshash_hybrid(buf, len, seeds[1]);
        uint64_t h2 = plumshash_hybrid(buf, len, seeds[2]);
        if (h0 != plumshash_hybrid(buf, len, seeds[0])) { printf("NONDETERMINISTIC len=%zu\n", len); fails++; }
        if (h0 == h1 || h0 == h2 || h1 == h2) dup++;
        if (h0 != h1 && h0 != h2 && h1 != h2) distinct++;
        /* adjacent lengths must differ for same seed (regression: 321 vs 322
         * collided before the length whitening fix) */
        if (len > 0 && h0 == plumshash_hybrid(buf, len - 1, seeds[0])) {
            printf("ADJACENT-LEN COLLISION len=%zu\n", len); fails++;
        }
    }
    printf("lens 0..4096: %d distinct-seed sets, %d same-hash triples\n", distinct, dup);

    /* 2) avalanche: flip each of first 8 bytes at 16B and 256B */
    size_t lens[2] = {16, 256};
    for (int li = 0; li < 2; li++) {
        size_t len = lens[li];
        uint64_t base = plumshash_hybrid(buf, len, 42);
        long total = 0; int worst = 0;
        for (int b = 0; b < 8; b++) {
            for (int bit = 0; bit < 8; bit++) {
                buf[b] ^= (uint8_t)(1u << bit);
                uint64_t h = plumshash_hybrid(buf, len, 42);
                buf[b] ^= (uint8_t)(1u << bit);
                int d = bitcount64(base ^ h);
                total += d; if (d < worst) worst = d;
            }
        }
        double avg = total / 64.0;
        printf("avalanche %zuB: avg %.2f/64 bits (ideal 32)\n", len, avg);
        if (avg < 25.0) { printf("  AVALANCHE TOO WEAK\n"); fails++; }
    }

    /* 3) 1-byte flips at every position, len 64 (tail-path coverage) */
    {
        size_t len = 64;
        uint64_t base = plumshash_hybrid(buf, len, 7);
        long total = 0;
        for (size_t b = 0; b < len; b++) {
            buf[b] ^= 0x5A;
            uint64_t h = plumshash_hybrid(buf, len, 7);
            buf[b] ^= 0x5A;
            total += bitcount64(base ^ h);
        }
        double avg = total / (double)len;
        printf("per-position flip len=64: avg %.2f/64 bits\n", avg);
        if (avg < 25.0) { printf("  TOO WEAK\n"); fails++; }
    }

    /* 4) seed whitening: seed=0 vs seed=1 on identical 256B key must differ */
    {
        uint64_t a = plumshash_hybrid(buf, 256, 0);
        uint64_t b = plumshash_hybrid(buf, 256, 1);
        printf("seed 0 vs 1 at 256B: %s\n", a != b ? "differ" : "SAME (BAD)");
        if (a == b) fails++;
    }

    /* 5) hybrid vs original plumshash at >=256B (expect differ — bulk
     * variant in hybrid is not the repo's current plums_fast) */
    {
        int same = 0, diff = 0;
        for (size_t len = 256; len <= 4096; len += 16) {
            if (plumshash_hybrid(buf, len, 5) == plumshash(buf, len, 5)) same++;
            else diff++;
        }
        printf(">=256B hybrid==plumshash: %d same, %d differ\n", same, diff);
    }

    free(buf);
    printf(fails ? "FAILED (%d)\n" : "ALL EDGE CHECKS PASSED\n", fails);
    return fails ? 1 : 0;
}
