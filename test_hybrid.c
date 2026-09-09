/*
 * test_hybrid.c — correctness + concurrency stress for hybrid_intern.h.
 *
 * Phase 1: single-threaded roundtrip, dedup, boundary sizes.
 * Phase 2: 8 threads hammering the SAME string set concurrently — asserts
 *          every thread resolves the SAME handle per string (no duplicate
 *          intern), roundtrip is exact, and all NS handles are distinct.
 * Phase 3: evict/gen — stale handles resolve NULL, re-intern bumps gen.
 * Phase 4: evict-during-resolve (TOCTOU) — a small pool forces slot reuse;
 *          evictor threads churn slots with different-length content while
 *          resolver threads read stable handles. The seqlock guarantees a
 *          resolve returns the intact original or NULL (stale), never a
 *          torn snapshot. str() is zero-copy (returns a pointer to
 *          immutable arena bytes), valid until pool destroy.
 *
 * Compile: clang -O3 -std=c11 -march=native -pthread -o test_hybrid test_hybrid.c
 */
#define PLUMSHASH_IMPLEMENTATION
#include "hybrid_intern.h"
#include <pthread.h>

#define NT 8
#define NS 2000
#define ITERS 30

static HybridPool *hp;
static char       strings[NS][256];
static uint32_t   lens[NS];
static SF_Handle  results[NT][NS];

static int handle_cmp(const void *a, const void *b) {
    uint64_t x = ((const SF_Handle *)a)->u, y = ((const SF_Handle *)b)->u;
    return (x > y) - (x < y);
}

static void *stress_thread(void *arg) {
    intptr_t tid = (intptr_t)arg;
    for (int it = 0; it < ITERS; it++)
        for (int i = 0; i < NS; i++)
            results[tid][i] = hybrid_intern(hp, strings[i], lens[i]);
    return NULL;
}

static int phase1(void) {
    const char *boundaries[] = {
        "", "a", "abc", "abcd", "0123456789abcdef",              /* 0,1,3,4,16 */
        "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA",    /* 48 (short max) */
        "BBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBB",    /* 49 (long min) */
        NULL
    };
    uint32_t rl = 0;
    for (int i = 0; boundaries[i]; i++) {
        uint32_t len = (uint32_t)strlen(boundaries[i]);
        SF_Handle h1 = hybrid_intern(hp, boundaries[i], len);
        SF_Handle h2 = hybrid_intern(hp, boundaries[i], len);
        if (h1.u != h2.u) { fprintf(stderr, "dedup fail len=%u\n", len); return 1; }
        const char *s = hybrid_str(hp, h1, &rl);
        if (!s || rl != len || memcmp(s, boundaries[i], len) != 0) {
            fprintf(stderr, "roundtrip fail len=%u\n", len); return 1;
        }
    }
    static char big[1000];
    for (int i = 0; i < 1000; i++) big[i] = (char)('a' + (i % 26));
    SF_Handle h = hybrid_intern(hp, big, 1000);
    const char *s = hybrid_str(hp, h, &rl);
    if (!s || rl != 1000 || memcmp(s, big, 1000)) { fprintf(stderr, "long roundtrip fail\n"); return 1; }
    return 0;
}

static int phase3(void) {
    const char *k = "evict-me-now";
    SF_Handle h1 = hybrid_intern(hp, k, (uint32_t)strlen(k));
    hybrid_evict(hp, h1);
    if (hybrid_str(hp, h1, NULL) != NULL) { fprintf(stderr, "stale handle not NULL after evict\n"); return 1; }
    SF_Handle h2 = hybrid_intern(hp, k, (uint32_t)strlen(k));
    if (h2.u == h1.u) { fprintf(stderr, "re-intern did not bump gen\n"); return 1; }
    uint32_t rl = 0;
    const char *s = hybrid_str(hp, h2, &rl);
    if (!s || rl != strlen(k) || memcmp(s, k, rl)) { fprintf(stderr, "re-intern roundtrip fail\n"); return 1; }
    return 0;
}

/* ---- Phase 4: evict-during-resolve ---- */
#define CHURN_N 256
static HybridPool *churn_hp;
static SF_Handle  churn_handles[CHURN_N];
static char       churn_strings[CHURN_N][64];
static uint32_t   churn_lens[CHURN_N];
static volatile int churn_stop;

static void *evict_thread(void *arg) {
    (void)arg;
    uint64_t x = 0x123456789ULL;
    while (!churn_stop) {
        x = x * 6364136223846793005ULL + 1;
        int i = (int)(x % CHURN_N);
        hybrid_evict(churn_hp, churn_handles[i]);
        /* re-intern a DIFFERENT-length, DIFFERENT-content string so slot
         * reuse changes the bytes a concurrent resolver would read */
        char tmp[64];
        uint32_t l = 1 + (uint32_t)((x >> 32) % 63);
        for (uint32_t j = 0; j < l; j++) tmp[j] = (char)('A' + ((x >> j) & 1));
        (void)hybrid_intern(churn_hp, tmp, l);
    }
    return NULL;
}

static void *resolve_thread(void *arg) {
    (void)arg;
    uint64_t x = 0x9abcdef01234567ULL;
    while (!churn_stop) {
        x = x * 6364136223846793005ULL + 1;
        int i = (int)(x % CHURN_N);
        uint32_t rl = 0;
        const char *s = hybrid_str(churn_hp, churn_handles[i], &rl);
        /* must be NULL (stale) or the INTACT original — never torn */
        if (s && (rl != churn_lens[i] || memcmp(s, churn_strings[i], churn_lens[i]) != 0)) {
            fprintf(stderr, "TOCTOU: torn read on handle %d (len %u vs %u)\n", i, rl, churn_lens[i]);
            abort();
        }
    }
    return NULL;
}

static int phase4(void) {
    /* small pool -> M=32 slots, but CHURN_N=256 strings -> heavy slot reuse */
    churn_hp = hybrid_create(3, 16, 16);
    for (int i = 0; i < CHURN_N; i++) {
        uint32_t len = 1 + (uint32_t)(i % 60);
        for (uint32_t j = 0; j < len; j++) churn_strings[i][j] = (char)('a' + ((i + j) % 26));
        churn_strings[i][len] = 0;
        churn_lens[i] = len;
        churn_handles[i] = hybrid_intern(churn_hp, churn_strings[i], len);
    }
    churn_stop = 0;
    pthread_t ev[2], rs[4];
    for (int t = 0; t < 2; t++) pthread_create(&ev[t], NULL, evict_thread, NULL);
    for (int t = 0; t < 4; t++) pthread_create(&rs[t], NULL, resolve_thread, NULL);
    struct timespec ts = {1, 0};
    nanosleep(&ts, NULL);
    churn_stop = 1;
    for (int t = 0; t < 2; t++) pthread_join(ev[t], NULL);
    for (int t = 0; t < 4; t++) pthread_join(rs[t], NULL);
    hybrid_destroy(churn_hp);
    return 0;
}

int main(void) {
    /* INJECTIVE content: an 8-byte tag unique to i (no two strings collide)
     * + a filler pattern. Lengths 8..192 cover both pools. */
    for (int i = 0; i < NS; i++) {
        int len = 8 + (i * 37) % 185;
        uint64_t tag = (uint64_t)(i + 1) * 0x9E3779B97F4A7C15ULL;
        for (int j = 0; j < 8; j++) strings[i][j] = (char)(tag >> (8 * j));
        for (int j = 8; j < len; j++) strings[i][j] = (char)('a' + ((i + j) % 26));
        strings[i][len] = '\0';
        lens[i] = (uint32_t)len;
    }

    hp = hybrid_create(1, NS * 2, NS * 2);
    if (phase1()) return 1;

    pthread_t th[NT];
    for (intptr_t t = 0; t < NT; t++) pthread_create(&th[t], NULL, stress_thread, (void *)t);
    for (int t = 0; t < NT; t++) pthread_join(th[t], NULL);

    for (int i = 0; i < NS; i++) {
        SF_Handle ref = results[0][i];
        for (int t = 1; t < NT; t++)
            if (results[t][i].u != ref.u) {
                fprintf(stderr, "DUPLICATE INTERN: string %d, thread %d disagrees\n", i, t);
                return 1;
            }
        uint32_t rl = 0;
        const char *s = hybrid_str(hp, ref, &rl);
        if (!s || rl != lens[i] || memcmp(s, strings[i], lens[i]) != 0) {
            fprintf(stderr, "roundtrip fail: string %d\n", i);
            return 1;
        }
    }

    {
        static SF_Handle sorted[NS];
        memcpy(sorted, results[0], sizeof(sorted));
        qsort(sorted, NS, sizeof(SF_Handle), handle_cmp);
        for (int i = 1; i < NS; i++)
            if (sorted[i].u == sorted[i-1].u) { fprintf(stderr, "handle collision at %d\n", i); return 1; }
    }

    if (phase3()) return 1;
    if (phase4()) return 1;

    hybrid_print_stats(hp);
    hybrid_destroy(hp);
    printf("ALL HYBRID INTERN TESTS PASSED\n");
    return 0;
}
