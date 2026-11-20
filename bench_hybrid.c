/*
 * bench_hybrid.c — throughput and latency benchmark for the hybrid_intern
 * string-intern pool.
 *
 * The hash itself is benchmarked by bench_compete.c / test_speed.c. This
 * file measures the POOL: the table probe, the arena copy, the mutex, and
 * the read path.
 *
 * The single-threaded and the multi-threaded measurements are kept strictly
 * apart, because they answer different questions:
 *
 *   single-threaded  — per-operation latency of one caller (insert path,
 *                      dedup path, lock-free read path). No contention, no
 *                      cross-core traffic. This is the number that moves
 *                      when the table layout changes.
 *   multi-threaded   — aggregate throughput of T callers sharing ONE pool.
 *                      This is the number that moves when the insert path
 *                      stops serializing on the pool mutex.
 *
 * Mixing them in one figure hides both: a mutex change barely shows up in a
 * single-threaded run, and cache-locality work is invisible in a
 * contended one.
 *
 * Usage:
 *   ./bench_hybrid            both sections (default)
 *   ./bench_hybrid st         single-threaded section only
 *   ./bench_hybrid mt         multi-threaded section only
 *
 * Env: SWEEP_MAX=<n> caps the single-threaded sweep (default 100000,
 * max 1000000).
 *
 * Every case prints one stable machine-readable line:
 *
 *   bench section=<st|mt> case=<name> [k=v ...] ns=<ns/op> ops_s=<ops/sec>
 *
 * Build: clang -O3 -std=c11 -march=native -pthread -o bench_hybrid bench_hybrid.c
 */
#define PLUMSHASH_IMPLEMENTATION
#include "hybrid_intern.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define MAX_STRINGS 1000000
#define STR_BYTES   32

static uint64_t rng_state = 0x9E3779B97F4A7C15ULL;

static uint32_t rnd(void) {
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 7;
    rng_state ^= rng_state << 17;
    return (uint32_t)(rng_state >> 32);
}

static double now_sec(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

/* Unique, fixed-width payloads: "s%08u" + padding, so every string has the
 * same length and the hash cost is comparable across the run. */
static void fill_strings(char *buf, uint32_t n, uint32_t width) {
    for (uint32_t i = 0; i < n; i++) {
        char *p = buf + (size_t)i * width;
        int k = snprintf(p, width, "s%08u", i);
        while (k < (int)width) p[k++] = '.';
        p[width - 1] = '\0';
    }
}

static void report(const char *section, const char *case_name, double secs,
                   uint64_t ops, const char *extra) {
    double ns = ops ? secs * 1e9 / (double)ops : 0.0;
    double ops_s = secs > 0 ? (double)ops / secs : 0.0;
    printf("bench section=%s case=%s ns=%.1f ops_s=%.0f%s%s\n",
           section, case_name, ns, ops_s,
           (extra && *extra) ? " " : "", extra ? extra : "");
    fflush(stdout);
}

/* ═══════════════════════════════════════════════════════════════════════
 * SINGLE-THREADED SECTION — per-operation latency, one caller
 * ═══════════════════════════════════════════════════════════════════════ */

/* intern: unique strings (insert path) */
static double bench_intern_miss(uint32_t n, char *strings, uint32_t width,
                                HybridPool *hp, uint64_t *ops_out,
                                char *extra, size_t extra_cap) {
    double t0 = now_sec();
    for (uint32_t i = 0; i < n; i++) {
        const char *s = strings + (size_t)i * width;
        if (hybrid_intern(hp, s, (uint32_t)strlen(s)).u == 0) {
            fprintf(stderr, "bench: intern failed at %u (pool full?)\n", i);
            break;
        }
    }
    double dt = now_sec() - t0;
    *ops_out = n;
    HybridStats st = hybrid_stats(hp);
    snprintf(extra, extra_cap, "entries=%u slots=%u collisions=%u",
             st.short_entries + st.long_entries,
             st.short_max + st.long_max,
             st.short_collisions + st.long_collisions);
    return dt;
}

/* intern: the same strings again (dedup path: probe + memcmp) */
static double bench_intern_hit(uint32_t n, char *strings, uint32_t width,
                               HybridPool *hp, uint64_t *ops_out,
                               char *extra, size_t extra_cap) {
    double t0 = now_sec();
    for (uint32_t i = 0; i < n; i++) {
        const char *s = strings + (size_t)i * width;
        hybrid_intern(hp, s, (uint32_t)strlen(s));
    }
    double dt = now_sec() - t0;
    *ops_out = n;
    HybridStats st = hybrid_stats(hp);
    snprintf(extra, extra_cap, "entries=%u",
             st.short_entries + st.long_entries);
    return dt;
}

/* lookup: lock-free read path over live handles */
static double bench_lookup(uint32_t n, char *strings, uint32_t width,
                           HybridPool *hp, uint64_t *ops_out,
                           char *extra, size_t extra_cap) {
    SF_Handle *h = malloc(sizeof(SF_Handle) * n);
    if (!h) return 0;
    uint32_t got = 0;
    for (uint32_t i = 0; i < n; i++) {
        const char *s = strings + (size_t)i * width;
        SF_Handle hh = hybrid_intern(hp, s, (uint32_t)strlen(s));
        if (hh.u) h[got++] = hh;
    }
    /* shuffle so the accesses do not follow insertion order */
    for (uint32_t i = got; i > 1; i--) {
        uint32_t j = rnd() % i;
        SF_Handle t = h[i - 1]; h[i - 1] = h[j]; h[j] = t;
    }
    volatile uint64_t sink = 0;
    double t0 = now_sec();
    for (uint32_t i = 0; i < got; i++) {
        const char *p = hybrid_str(hp, h[i], NULL);
        sink += (uint64_t)(uintptr_t)p;
    }
    double dt = now_sec() - t0;
    *ops_out = got;
    snprintf(extra, extra_cap, "hits=%u", got);
    free(h);
    (void)sink;
    return dt;
}

/* long strings: hash + arena copy dominate */
static void bench_long_string(uint32_t size, uint32_t count) {
    char *buf = malloc(size);
    if (!buf) return;
    memset(buf, 'x', size);
    buf[size - 1] = '\0';

    HYBRID_SCOPED(hp, 31, count, count);
    uint64_t ops = 0;
    char extra[128];
    char full[192];

    double dt = 0;
    for (uint32_t i = 0; i < count; i++) {
        buf[8] = (char)('a' + (i % 26));           /* keep the strings distinct */
        double t0 = now_sec();
        SF_Handle h = hybrid_intern(hp, buf, size);
        dt += now_sec() - t0;
        ops++;
        if (h.u == 0) break;
    }
    snprintf(extra, sizeof extra, "size=%u count=%u", size, count);
    report("st", "long_string_miss", dt, ops, extra);

    dt = 0;
    for (uint32_t i = 0; i < count; i++) {
        buf[8] = (char)('a' + (i % 26));
        double t0 = now_sec();
        hybrid_intern(hp, buf, size);
        dt += now_sec() - t0;
        ops++;
    }
    snprintf(full, sizeof full, "%s", extra);
    report("st", "long_string_hit", dt, ops, full);
    free(buf);
}

static void run_single_threaded(uint32_t sweep_max) {
    printf("## single-threaded (per-operation latency, one caller)\n");
    uint32_t sizes[3] = { 10000, 100000, 1000000 };
    for (int si = 0; si < 3; si++) {
        uint32_t n = sizes[si];
        if (n > sweep_max) continue;

        char *strings = malloc((size_t)n * STR_BYTES);
        if (!strings) break;
        fill_strings(strings, n, STR_BYTES);

        HYBRID_SCOPED(hp, 1, n, n);
        uint64_t ops = 0;
        char extra[160];
        char full[192];

        double dt = bench_intern_miss(n, strings, STR_BYTES, hp, &ops, extra, sizeof extra);
        snprintf(full, sizeof full, "n=%u %s", n, extra);
        report("st", "intern_miss", dt, ops, full);

        dt = bench_intern_hit(n, strings, STR_BYTES, hp, &ops, extra, sizeof extra);
        snprintf(full, sizeof full, "n=%u %s", n, extra);
        report("st", "intern_hit", dt, ops, full);

        dt = bench_lookup(n, strings, STR_BYTES, hp, &ops, extra, sizeof extra);
        snprintf(full, sizeof full, "n=%u %s", n, extra);
        report("st", "lookup", dt, ops, full);

        free(strings);
    }

    bench_long_string(1024, 200);
    bench_long_string(65536, 50);
}

/* ═══════════════════════════════════════════════════════════════════════
 * MULTI-THREADED SECTION — aggregate throughput, T callers, one pool
 * ═══════════════════════════════════════════════════════════════════════ */

typedef struct {
    HybridPool *hp;
    char       *strings;
    uint32_t    width;
    uint32_t    count;
    uint64_t    ops;
} ThreadArg;

static void *thread_worker(void *arg) {
    ThreadArg *a = (ThreadArg *)arg;
    for (uint32_t i = 0; i < a->count; i++) {
        const char *s = a->strings + (size_t)i * a->width;
        if (hybrid_intern(a->hp, s, (uint32_t)strlen(s)).u == 0) break;
        a->ops++;
    }
    return NULL;
}

static void bench_threads(uint32_t nthreads, uint32_t per_thread) {
    char *strings = malloc((size_t)per_thread * nthreads * STR_BYTES);
    if (!strings) return;
    uint32_t total = per_thread * nthreads;
    fill_strings(strings, total, STR_BYTES);

    HYBRID_SCOPED(hp, 32, total, total);
    pthread_t th[64];
    ThreadArg args[64];
    if (nthreads > 64) nthreads = 64;

    double t0 = now_sec();
    for (uint32_t t = 0; t < nthreads; t++) {
        args[t].hp = hp;
        args[t].strings = strings + (size_t)t * per_thread * STR_BYTES;
        args[t].width = STR_BYTES;
        args[t].count = per_thread;
        args[t].ops = 0;
        pthread_create(&th[t], NULL, thread_worker, &args[t]);
    }
    uint64_t ops = 0;
    for (uint32_t t = 0; t < nthreads; t++) {
        pthread_join(th[t], NULL);
        ops += args[t].ops;
    }
    double dt = now_sec() - t0;

    char extra[128];
    HybridStats st = hybrid_stats(hp);
    snprintf(extra, sizeof extra, "threads=%u entries=%u", nthreads,
             st.short_entries + st.long_entries);
    report("mt", "threads_intern", dt, ops, extra);
    free(strings);
}

static void run_multi_threaded(void) {
    printf("## multi-threaded (aggregate throughput, T callers, one pool)\n");
    bench_threads(1, 50000);
    bench_threads(2, 25000);
    bench_threads(4, 12500);
    bench_threads(8, 6250);
}

int main(int argc, char **argv) {
    const char *mode = (argc > 1) ? argv[1] : "all";
    uint32_t sweep_max = 100000;
    const char *env = getenv("SWEEP_MAX");
    if (env) sweep_max = (uint32_t)strtoul(env, NULL, 10);
    if (sweep_max > MAX_STRINGS) sweep_max = MAX_STRINGS;

    printf("# plumshash intern bench\n");

    if (!strcmp(mode, "st") || !strcmp(mode, "all")) run_single_threaded(sweep_max);
    if (!strcmp(mode, "mt") || !strcmp(mode, "all")) run_multi_threaded();
    return 0;
}
