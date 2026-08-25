/* verify_identical.c — new hybrid vs v3 backup.
 * Value-identical everywhere EXCEPT len 113-224, where the dispatch now
 * routes to the mum 7-lane path (113-128: was mid; 129-224: was the
 * xxh3-style large walk). All other lengths must be byte-identical. */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stddef.h>

uint64_t old_hash(const void *b, size_t l, uint64_t s);
uint64_t new_hash(const void *b, size_t l, uint64_t s);

static uint64_t rng_state = 0xDEADBEEFCAFEF00DULL;
static uint64_t rng(void) {
    rng_state ^= rng_state << 13; rng_state ^= rng_state >> 7;
    rng_state ^= rng_state << 17; return rng_state;
}

int main(void) {
    uint8_t *buf = (uint8_t*)malloc(4096 + 64);
    if (!buf) return 1;
    for (int i = 0; i < 4096 + 64; i++) buf[i] = (uint8_t)rng();

    uint64_t seeds[] = {0, 1, 42, 0xDEADBEEFCAFEF00DULL, rng(), rng()};
    int nseed = (int)(sizeof(seeds) / sizeof(seeds[0]));
    long mism = 0, total = 0, expected_diff = 0;

    for (int off = 0; off <= 8; off += 8) {
        for (size_t len = 0; len <= 1024; len++) {
            for (int s = 0; s < nseed; s++) {
                uint64_t a = old_hash(buf + off, len, seeds[s]);
                uint64_t b = new_hash(buf + off, len, seeds[s]);
                total++;
                int window = (len >= 113 && len <= 224);
                if (a != b) {
                    if (window) { expected_diff++; continue; }
                    if (mism < 10)
                        printf("MISMATCH off=%d len=%zu seed=%llu: old=%016llx new=%016llx\n",
                               off, len, (unsigned long long)seeds[s],
                               (unsigned long long)a, (unsigned long long)b);
                    mism++;
                } else if (window) {
                    if (mism < 10) printf("UNEXPECTED-SAME in window len=%zu (seed=%llu)\n",
                                          len, (unsigned long long)seeds[s]);
                    mism++;
                }
            }
        }
    }
    for (size_t len = 1025; len <= 8192; len += 17) {
        for (int s = 0; s < nseed; s++) {
            uint64_t a = old_hash(buf, len, seeds[s]);
            uint64_t b = new_hash(buf, len, seeds[s]);
            total++;
            if (a != b) mism++;
        }
    }
    printf("%ld comparisons: %ld mismatches, %ld expected-diffs (113-224 window)\n",
           total, mism, expected_diff);
    free(buf);
    return mism ? 1 : 0;
}

