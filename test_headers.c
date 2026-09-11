/*
 * test_headers.c — the v1.2.0 integration path: ONE translation unit using
 * plumshash.h and hybrid_intern.h together.
 *
 * Regression this pins: plumshash.h's implementation block lives outside its
 * include guard, so a second include in the same TU used to re-emit every
 * static definition (20 redefinition errors). hybrid_intern.h includes
 * plumshash.h, so `#include "plumshash.h"` followed by
 * `#include "hybrid_intern.h"` — the natural way to use both — did not compile.
 * The implementation section now carries its own once-guard
 * (PLUMSHASH_IMPLEMENTATION_ONCE).
 *
 * Also checks the two version macros and that both APIs work in that TU.
 *
 * Compile: clang -O3 -march=native -Wall -Wextra -pthread -o test_headers test_headers.c
 */
#define PLUMSHASH_IMPLEMENTATION
#include "plumshash.h"      /* implementation is emitted here */
#include "hybrid_intern.h"  /* includes plumshash.h again -> must be a no-op */

#include <stdio.h>
#include <string.h>

int main(void) {
    int fail = 0;
    const char *key = "hello, world";
    uint64_t h = plumshash(key, 13, 0);

    printf("  plumshash v%s / hybrid_intern v%s\n",
           PLUMSHASH_VERSION, HYBRID_INTERN_VERSION);
    printf("  hash(\"hello, world\", seed 0) = %016llx\n", (unsigned long long)h);

    if (h == 0) { printf("  FAIL: hash is zero\n"); fail++; }
    if (strcmp(PLUMSHASH_VERSION, "1.2.0") != 0) {
        printf("  FAIL: PLUMSHASH_VERSION=%s (want 1.2.0)\n", PLUMSHASH_VERSION);
        fail++;
    }

    /* both APIs from the same TU */
    HybridPool *hp = hybrid_create(1, 256, 64);
    SF_Handle a = hybrid_intern(hp, key, 13);
    SF_Handle b = hybrid_intern(hp, key, 13);
    uint32_t len = 0;
    const char *s = hybrid_str(hp, a, &len);

    if (a.u != b.u) { printf("  FAIL: intern not idempotent\n"); fail++; }
    if (!s || len != 13 || memcmp(s, key, 13) != 0) { printf("  FAIL: intern roundtrip\n"); fail++; }

    SF_Handle t = hybrid_intern_scratch(hp, "scratch", 7);
    hybrid_epoch_end(hp);
    if (hybrid_str(hp, t, NULL) != NULL) { printf("  FAIL: scratch survived the epoch boundary\n"); fail++; }

    hybrid_destroy(hp);

    if (fail) { printf("HEADER INTEGRATION: %d FAILED\n", fail); return 1; }
    printf("HEADER INTEGRATION PASSED (both headers, one TU)\n");
    return 0;
}
