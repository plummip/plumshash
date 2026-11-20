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
 * single-threaded run, and cache-locality work is invisible in a contended
 * one.
 *
 * Every case is run REPS times and the BEST (minimum ns/op) is reported:
 * a single pass on a phone is noisy enough to invent a 30% regression.
 *
 * Usage:
 *   ./bench_hybrid            both sections (default)
 *   ./bench_hybrid st         single-threaded section only
 *   ./bench_hybrid mt         multi-threaded section only
 *
 * Env: SWEEP_MAX=<n>  caps the single-threaded sweep (default 100000,
 *                       max 1000000)
 *       REPS=<n>       repetitions per case (default 5)
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
static int      reps      = 5;

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

/* Unique, fixed-width payloads: "s%08u" + a repetition tag + padding, so
 * every string has the same length and every repetition interns NEW
 * strings (a repeat must be a miss, not a dedup). */
static void fill_strings(char *buf, uint32_t n, uint32_t width, int rep) {
    for (uint32_t i = 0; i < n; i++) {
        char *p = buf + (size_t)i * width;
        int k = snprintf(p, width, "s%08u", i);
        k += snprintf(p + k, width - (size_t)k, "r%03d", rep);
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

/* intern: unique strings (insert path). Returns the best ns/op.
 *
 * Each repetition gets a FRESH pool: the strings of repetition r are new
 * (so every intern is a miss), and the pool is created outside the timed
 * region — sizing and zeroing the table is not part of the measurement. */
static double bench_intern_miss(uint32_t n, char *strings, uint32_t width,
                                uint16_t pool_id, uint64_t *ops_out,
                                char *extra, size_t extra_cap) {
    double best = 1e30;
    uint64_t ops = 0;
    for (int r = 0; r < reps; r++) {
        fill_strings(strings, n, width, r);
        HybridPool *hp = hybrid_create(pool_id, n, n);
        if (!hp) break;
        double t0 = now_sec();
        uint64_t done = 0;
        for (uint32_t i = 0; i < n; i++) {
            const char *s = strings + (size_t)i * width;
            if (hybrid_intern(hp, s, (uint32_t)strlen(s)).u == 0) break;
            done++;
        }
        double dt = now_sec() - t0;
        if (done == n && dt < best) best = dt;
        ops = done;
        HybridStats st = hybrid_stats(hp);
        snprintf(extra, extra_cap, "entries=%u slots=%u collisions=%u",
                 st.short_entries + st.long_entries,
                 st.short_max + st.long_max,
                 st.short_collisions + st.long_collisions);
        hybrid_destroy(hp);
        if (done != n) {
            fprintf(stderr, "bench: intern failed at %llu (pool full?)\n",
                    (unsigned long long)done);
            break;
        }
    }
    *ops_out = ops;
    return best;
}

/* intern: the same strings again (dedup path: probe + memcmp) */
static double bench_intern_hit(uint32_t n, char *strings, uint32_t width,
                               HybridPool *hp, uint64_t *ops_out,
                               char *extra, size_t extra_cap) {
    double best = 1e30;
    uint64_t ops = 0;
    volatile uint64_t sink = 0;
    for (int r = 0; r < reps; r++) {
        fill_strings(strings, n, width, 0);
        for (uint32_t i = 0; i < n; i++)
            sink += hybrid_intern(hp, strings + (size_t)i * width,
                                 (uint32_t)strlen(strings + (size_t)i * width)).u;
        double t0 = now_sec();
        uint64_t hsum = 0;
        for (uint32_t i = 0; i < n; i++) {
            const char *s = strings + (size_t)i * width;
            hsum += hybrid_intern(hp, s, (uint32_t)strlen(s)).u;
        }
        double dt = now_sec() - t0;
        sink += hsum;   /* keep the work alive */
        if (dt < best) best = dt;
        ops = n;
    }
    *ops_out = ops;
    HybridStats st = hybrid_stats(hp);
    snprintf(extra, extra_cap, "entries=%u",
             st.short_entries + st.long_entries);
    return best;
}

/* lookup: lock-free read path over live handles */
static double bench_lookup(uint32_t n, char *strings, uint32_t width,
                           HybridPool *hp, uint64_t *ops_out,
                           char *extra, size_t extra_cap) {
    SF_Handle *h = malloc(sizeof(SF_Handle) * n);
    if (!h) return 0;
    uint32_t got = 0;
    fill_strings(strings, n, width, 0);
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
    double best = 1e30;
    for (int r = 0; r < reps; r++) {
        double t0 = now_sec();
        for (uint32_t i = 0; i < got; i++) {
            const char *p = hybrid_str(hp, h[i], NULL);
            sink += (uint64_t)(uintptr_t)p;
        }
        double dt = now_sec() - t0;
        if (dt < best) best = dt;
    }
    *ops_out = got;
    snprintf(extra, extra_cap, "hits=%u", got);
    free(h);
    (void)sink;
    return best;
}

/* long strings: hash + arena copy dominate */
static void bench_long_string(uint32_t size, uint32_t count) {
    char *buf = malloc(size);
    if (!buf) return;
    memset(buf, 'x', size);
    buf[size - 1] = '\0';

    HYBRID_SCOPED(hp, 31, count * (uint32_t)reps, count * (uint32_t)reps);
    char extra[128];

    double best = 1e30;
    for (int r = 0; r < reps; r++) {
        double dt = 0;
        for (uint32_t i = 0; i < count; i++) {
            buf[8] = (char)('a' + (i % 26));           /* keep the strings distinct */
            double t0 = now_sec();
            SF_Handle h = hybrid_intern(hp, buf, size);
            dt += now_sec() - t0;
            if (h.u == 0) break;
        }
        if (dt < best) best = dt;
    }
    snprintf(extra, sizeof extra, "size=%u count=%u", size, count);
    report("st", "long_string_miss", best, count, extra);

    best = 1e30;
    for (int r = 0; r < reps; r++) {
        double dt = 0;
        for (uint32_t i = 0; i < count; i++) {
            /* the SAME string as the miss pass — this is a dedup, not a
             * second insert */
            double t0 = now_sec();
            hybrid_intern(hp, buf, size);
            dt += now_sec() - t0;
        }
        if (dt < best) best = dt;
    }
    report("st", "long_string_hit", best, count, extra);
    free(buf);
}

/* The scratch (T0) tier: intern into it, then drop the whole tier in bulk
 * at the epoch boundary. This is the tier a compiler uses for per-statement
 * or per-pass temporaries, so the boundary cost matters. */
static void bench_scratch(uint32_t per_epoch, uint32_t epochs) {
    char *strings = malloc((size_t)per_epoch * STR_BYTES);
    if (!strings) return;

    double best_intern = 1e30, best_epoch = 1e30;
    uint64_t ops = 0, cleared = 0;
    uint32_t slots = 0;
    volatile uint64_t sink = 0;
    for (int r = 0; r < reps; r++) {
        for (uint32_t e = 0; e < epochs; e++) {
            fill_strings(strings, per_epoch, STR_BYTES, r * 1000 + (int)e);
            HYBRID_SCOPED(hp, 34, per_epoch, per_epoch);
            double t0 = now_sec();
            uint64_t hsum = 0;
            for (uint32_t i = 0; i < per_epoch; i++)
                hsum += hybrid_intern_scratch(hp, strings + (size_t)i * STR_BYTES,
                                              STR_BYTES - 1).u;
            double dt_intern = now_sec() - t0;
            sink += hsum;   /* keep the work alive */
            ops += per_epoch;

            double t1 = now_sec();
            uint32_t c = hybrid_epoch_end(hp);
            double dt_epoch = now_sec() - t1;
            cleared += c;
            slots = hp->short_pool->M;

            if (dt_intern < best_intern) best_intern = dt_intern;
            if (dt_epoch < best_epoch) best_epoch = dt_epoch;
        }
    }
    free(strings);

    char extra[128];
    /* best_intern is ONE epoch's elapsed time, so the divisor is one
     * epoch's entries — not the total over every rep and epoch. */
    snprintf(extra, sizeof extra, "per_epoch=%u slots=%u", per_epoch, slots);
    report("st", "scratch_intern", best_intern, per_epoch, extra);
    (void)sink;
    /* the boundary cost, amortised over the entries it dropped */
    snprintf(extra, sizeof extra, "per_epoch=%u slots=%u cleared=%llu ns_per_entry=%.1f",
             per_epoch, slots, (unsigned long long)cleared,
             best_epoch * 1e9 / (double)per_epoch);
    report("st", "epoch_end", best_epoch, per_epoch, extra);
}

static void run_single_threaded(uint32_t sweep_max) {
    printf("## single-threaded (per-operation latency, one caller)\n");
    uint32_t sizes[3] = { 10000, 100000, 1000000 };
    for (int si = 0; si < 3; si++) {
        uint32_t n = sizes[si];
        if (n > sweep_max) continue;

        char *strings = malloc((size_t)n * STR_BYTES);
        if (!strings) break;

        uint64_t ops = 0;
        char extra[160];
        char full[192];

        double dt = bench_intern_miss(n, strings, STR_BYTES, 1, &ops, extra, sizeof extra);
        snprintf(full, sizeof full, "n=%u %s", n, extra);
        report("st", "intern_miss", dt, ops, full);

        HYBRID_SCOPED(hp, 1, n, n);
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
    bench_scratch(10000, 4);
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
    uint32_t total = per_thread * nthreads;
    char *strings = malloc((size_t)total * STR_BYTES);
    if (!strings) return;

    double best = 1e30;
    uint64_t ops = 0;
    for (int r = 0; r < reps; r++) {
        fill_strings(strings, total, STR_BYTES, r);
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
        uint64_t done = 0;
        for (uint32_t t = 0; t < nthreads; t++) {
            pthread_join(th[t], NULL);
            done += args[t].ops;
        }
        double dt = now_sec() - t0;
        if (dt < best) best = dt;
        ops = done;
    }
    free(strings);

    char extra[128];
    snprintf(extra, sizeof extra, "threads=%u ops=%llu", nthreads,
             (unsigned long long)ops);
    report("mt", "threads_intern", best, ops, extra);
}

/* The same set interned by every thread: the dedup path. This is where the
 * lock-free probe matters — the string is already interned, so the lookup
 * never needs the mutex. */
static void bench_threads_shared(uint32_t nthreads, uint32_t per_thread) {
    uint32_t total = per_thread;
    char *strings = malloc((size_t)total * STR_BYTES);
    if (!strings) return;

    double best = 1e30;
    uint64_t ops = 0;
    for (int r = 0; r < reps; r++) {
        fill_strings(strings, total, STR_BYTES, r);
        HYBRID_SCOPED(hp, 33, total, total);
        pthread_t th[64];
        ThreadArg args[64];
        if (nthreads > 64) nthreads = 64;

        double t0 = now_sec();
        for (uint32_t t = 0; t < nthreads; t++) {
            args[t].hp = hp;
            args[t].strings = strings;            /* everyone interns the SAME set */
            args[t].width = STR_BYTES;
            args[t].count = per_thread;
            args[t].ops = 0;
            pthread_create(&th[t], NULL, thread_worker, &args[t]);
        }
        uint64_t done = 0;
        for (uint32_t t = 0; t < nthreads; t++) {
            pthread_join(th[t], NULL);
            done += args[t].ops;
        }
        double dt = now_sec() - t0;
        if (dt < best) best = dt;
        ops = done;
    }
    free(strings);

    char extra[128];
    snprintf(extra, sizeof extra, "threads=%u ops=%llu shared=1", nthreads,
             (unsigned long long)ops);
    report("mt", "threads_intern_shared", best, ops, extra);
}

static void run_multi_threaded(void) {
    printf("## multi-threaded (aggregate throughput, T callers, one pool)\n");
    bench_threads(1, 50000);
    bench_threads(2, 25000);
    bench_threads(4, 12500);
    bench_threads(8, 6250);
    bench_threads_shared(1, 50000);
    bench_threads_shared(2, 25000);
    bench_threads_shared(4, 12500);
    bench_threads_shared(8, 6250);
}

int main(int argc, char **argv) {
    const char *mode = (argc > 1) ? argv[1] : "all";
    uint32_t sweep_max = 100000;
    const char *env = getenv("SWEEP_MAX");
    if (env) sweep_max = (uint32_t)strtoul(env, NULL, 10);
    if (sweep_max > MAX_STRINGS) sweep_max = MAX_STRINGS;
    env = getenv("REPS");
    if (env) { reps = (int)strtol(env, NULL, 10); if (reps < 1) reps = 1; }

    /* Warm the CPU up: the first case measured would otherwise run at a
     * lower clock and look 30% slower than it is. */
    {
        volatile uint64_t sink = 0;
        double t0 = now_sec();
        while (now_sec() - t0 < 2.0) {
            for (int i = 0; i < 100000; i++) sink += (uint64_t)i;
        }
        (void)sink;
    }

    printf("# plumshash intern bench (reps=%d)\n", reps);

    if (!strcmp(mode, "st") || !strcmp(mode, "all")) run_single_threaded(sweep_max);
    if (!strcmp(mode, "mt") || !strcmp(mode, "all")) run_multi_threaded();
    return 0;
}
