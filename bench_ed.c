/*
 * bench_ed.c — Correct, fair benchmark of edit-distance algorithms.
 *
 * Compares 5 edit-distance implementations with proper methodology:
 *   - xorshift64 PRNG for reproducible test pairs
 *   - clock_gettime(CLOCK_MONOTONIC) for nanosecond timing
 *   - Warmup phase before timed runs
 *   - DCE barrier (volatile sink)
 *   - Correctness verified before any timing
 *
 * Algorithms:
 *   1. Myers bit-parallel (corrected early-termination)
 *   2. Myers bit-parallel (reference — buggy, from fastdb.c)
 *   3. Damerau-Levenshtein with early termination
 *   4. Hybrid (Myers → DL fallback)
 *   5. Wagner-Fischer baseline
 *
 * Scenarios:
 *   A. Random strings (worst case: max_k=3, most pairs not similar)
 *   B. Near-match strings (realistic: trigram-filtered candidates)
 *   C. Stratified by string length (4-32 chars)
 *
 * Compile: gcc -O3 -march=armv8-a -o bench_ed bench_ed.c
 */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <time.h>

#define MYERS_MAX 62  /* max safe chars for 64-bit Myers (63 bits + overflow guard) */

/* ── xorshift64 PRNG ── */
static uint64_t xs64_state = 42;
static uint64_t xs64(void) {
    uint64_t x = xs64_state;
    x ^= x << 13; x ^= x >> 7; x ^= x << 17;
    return xs64_state = x;
}

/* ── Myers bit-parallel (reference — has premature kill bug) ── */
static int myers_ref(const char *a, int la, const char *b, int lb, int max_k) {
    if (la > MYERS_MAX || lb > MYERS_MAX) return -1;
    if (la > lb) { const char *t = a; a = b; b = t; int tl = la; la = lb; lb = tl; }
    if (lb - la > max_k) return max_k + 1;
    uint64_t Peq[256] = {0};
    for (int i = 0; i < la; i++) Peq[(uint8_t)a[i]] |= (1ULL << i);
    uint64_t Pv = (1ULL << la) - 1, Mv = 0;
    int score = la;
    for (int j = 0; j < lb; j++) {
        uint64_t Eq = Peq[(uint8_t)b[j]];
        uint64_t Xv = Eq | Mv;
        uint64_t Xh = (((Eq & Pv) + Pv) ^ Pv) | Eq;
        uint64_t Ph = Mv | ~(Xh | Pv);
        uint64_t Mh = Pv & Xh;
        if (Ph & (1ULL << (la - 1))) score++;
        if (Mh & (1ULL << (la - 1))) score--;
        Ph = (Ph << 1) | 1;
        Pv = (Mh << 1) | ~(Xv | Ph);
        Mv = Ph & Xv;
        if (score > max_k) return max_k + 1;
    }
    return score;
}

/* ── Myers bit-parallel (corrected) ── */
static int myers(const char *a, int la, const char *b, int lb, int max_k) {
    if (la > MYERS_MAX || lb > MYERS_MAX) return -1;
    if (la > lb) { const char *t = a; a = b; b = t; int tl = la; la = lb; lb = tl; }
    if (lb - la > max_k) return max_k + 1;
    uint64_t Peq[256] = {0};
    for (int i = 0; i < la; i++) Peq[(uint8_t)a[i]] |= (1ULL << i);
    uint64_t Pv = (1ULL << la) - 1, Mv = 0;
    int score = la;
    for (int j = 0; j < lb; j++) {
        uint64_t Eq = Peq[(uint8_t)b[j]];
        uint64_t Xv = Eq | Mv;
        uint64_t Xh = (((Eq & Pv) + Pv) ^ Pv) | Eq;
        uint64_t Ph = Mv | ~(Xh | Pv);
        uint64_t Mh = Pv & Xh;
        if (Ph & (1ULL << (la - 1))) score++;
        if (Mh & (1ULL << (la - 1))) score--;
        Ph = (Ph << 1) | 1;
        Pv = (Mh << 1) | ~(Xv | Ph);
        Mv = Ph & Xv;
        int remaining = lb - j - 1;
        if (score - remaining > max_k) return max_k + 1;
    }
    return score;
}

/* ── Damerau-Levenshtein ── */
static int damerau(const char *a, int la, const char *b, int lb, int max_k) {
    if (la > 60) la = 60; if (lb > 60) lb = 60;
    int ld = la > lb ? la - lb : lb - la;
    if (ld > max_k) return max_k + 1;
    int d0[64], d1[64], d2[64], *p0=d0, *p1=d1, *p2=d2;
    for (int j = 0; j <= lb; j++) p1[j] = j;
    for (int i = 1; i <= la; i++) {
        p0[0] = i; int row_min = i;
        for (int j = 1; j <= lb; j++) {
            int cost = (a[i-1] == b[j-1]) ? 0 : 1;
            int m = p1[j] + 1;
            if (p0[j-1] + 1 < m) m = p0[j-1] + 1;
            if (p1[j-1] + cost < m) m = p1[j-1] + cost;
            if (i>1 && j>1 && a[i-1]==b[j-2] && a[i-2]==b[j-1])
                if (p2[j-2] + cost < m) m = p2[j-2] + cost;
            p0[j] = m; if (m < row_min) row_min = m;
        }
        if (row_min > max_k) return max_k + 1;
        int *t = p2; p2 = p1; p1 = p0; p0 = t;
    }
    return p1[lb];
}

/* ── Hybrid (Myers corrected → DL fallback) ── */
static int hybrid(const char *a, int la, const char *b, int lb, int max_k) {
    int r = myers(a, la, b, lb, max_k);
    if (r >= 0 && r <= max_k) return r;
    if (r == -1) return damerau(a, la, b, lb, max_k);
    return damerau(a, la, b, lb, max_k);
}

/* ── Wagner-Fischer baseline ── */
static int wagner_fischer(const char *a, int la, const char *b, int lb, int max_k) {
    int diff = la - lb; if (diff < 0) diff = -diff;
    if (diff > max_k) return max_k + 1;
    int d0[128], d1[128], *prev = d0, *cur = d1;
    for (int j = 0; j <= lb; j++) prev[j] = j;
    for (int i = 1; i <= la; i++) {
        cur[0] = i; int row_min = i;
        for (int j = 1; j <= lb; j++) {
            int cost = (a[i-1] == b[j-1]) ? 0 : 1;
            int m = prev[j] + 1;
            if (cur[j-1] + 1 < m) m = cur[j-1] + 1;
            if (prev[j-1] + cost < m) m = prev[j-1] + cost;
            cur[j] = m;
            if (m < row_min) row_min = m;
        }
        if (row_min > max_k) return max_k + 1;
        int *t = prev; prev = cur; cur = t;
    }
    return prev[lb];
}

/* ── Benchmark infrastructure ── */
typedef int (*ed_fn)(const char*,int,const char*,int,int);

#define N_PAIRS   1000
#define WARMUP     200
#define REPEATS    100
#define MAX_LEN     32
#define MAX_K        3

/* Generate random string of given length */
static void rand_str(char *s, int len) {
    for (int i = 0; i < len; i++) s[i] = 'a' + (int)(xs64() % 26);
    s[len] = 0;
}

/* Mutate: apply `k` random edits to produce a near-match */
static int mutate(const char *src, int slen, char *dst, int k) {
    /* Copy */
    memcpy(dst, src, slen);
    int dlen = slen;
    for (int e = 0; e < k; e++) {
        int op = (int)(xs64() % 3);
        if (dlen <= 1) op = 1; /* can't delete too-short string */
        if (dlen >= 62) op = 2; /* can't insert into too-long */
        int pos = dlen > 0 ? (int)(xs64() % dlen) : 0;
        switch (op) {
        case 0: /* substitution */
            dst[pos] = 'a' + (int)(xs64() % 26);
            break;
        case 1: /* deletion */
            if (pos < dlen - 1) memmove(dst+pos, dst+pos+1, dlen-pos-1);
            dlen--;
            break;
        case 2: /* insertion */
            if (dlen < 62) {
                memmove(dst+pos+1, dst+pos, dlen-pos);
                dst[pos] = 'a' + (int)(xs64() % 26);
                dlen++;
            }
            break;
        }
    }
    dst[dlen] = 0;
    return dlen;
}

/* Run one benchmark and print result */
static double bench_one(const char *name, ed_fn fn,
                         const char **a_strs, const int *a_lens,
                         const char **b_strs, const int *b_lens,
                         int n, int max_k, int warmup, int repeats) {
    volatile int sink = 0;
    int total = 0;
    struct timespec t0, t1;

    for (int w = 0; w < warmup; w++) {
        int i = (w * 7 + 1) % n;
        int j = (w * 13 + 5) % n;
        sink += fn(a_strs[i], a_lens[i], b_strs[j], b_lens[j], max_k);
    }

    clock_gettime(CLOCK_MONOTONIC, &t0);
    for (int r = 0; r < repeats; r++) {
        for (int i = 0; i < n; i++) {
            int j = (i * 3 + 1) % n;
            sink += fn(a_strs[i], a_lens[i], b_strs[j], b_lens[j], max_k);
            total++;
        }
    }
    clock_gettime(CLOCK_MONOTONIC, &t1);

    double dt = (t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) / 1e9;
    printf("  %-26s  %10.0f ops/s  %8.0f ns/op\n",
           name, total / dt, dt / total * 1e9);
    (void)sink;
    return dt;
}

int main(void) {
    /* ── Correctness ── */
    struct { const char *a,*b; int ed; const char *desc; } c_tests[] = {
        {"abcd","abcde",1,"insertion end"},
        {"abc","abxc",1,"insertion mid"},
        {"amsterdm","amsterdam",1,"transposition"},
        {"roterdam","rotterdam",1,"deletion"},
        {"xyz","xyz",0,"exact"},
        {"abc","def",3,"all different"},
        {"","abc",3,"empty string"},
        {"kitten","sitting",3,"classic"},
        {"abcdefgh","abcdefgh",0,"8-char exact"},
        {"abcdefgh","abcXefgh",1,"8-char sub"},
        {"abcdefghijklmnop","abcdefghijklmnop",0,"16-char exact"},
        {"abcdefghijklmnop","abcXefghijklmnop",1,"16-char sub"},
        {"a","b",1,"1-char"},
        {"ab","ba",2,"swap (M=2,DL=1)*"},
        {"floccinaucinihilipilification","floccinaucinihilipilification",0,"29-char exact"},
        {"","",0,"both empty"},
        {"abc","",3,"abc vs empty"},
        /* Myers limit: 62 safe chars (63 bits set = max for uint64_t without overflow).
         * At 63 chars, intermediate computations in (Eq&Pv)+Pv need bit 64. */
        {"abcdefghijklmnopqrstuvwxyz0123456789abcdefghijklmnopqrstuvwxyz",
         "abcdefghijklmnopqrstuvwxyz0123456789abcdefghijklmnopqrstuvwxyz",0,"62-char exact"},
        {"abcdefghijklmnopqrstuvwxyz0123456789abcdefghijklmnopqrstuvwxyz",
         "Xbcdefghijklmnopqrstuvwxyz0123456789abcdefghijklmnopqrstuvwxyz",1,"62-char sub"},
        {NULL,NULL,0,NULL}
    };

    int pass = 0, fail = 0;
    printf("=== Correctness (max_k=5) ===\n");
    for (int i = 0; c_tests[i].a; i++) {
        int la = strlen(c_tests[i].a), lb = strlen(c_tests[i].b);
        int mr = myers_ref(c_tests[i].a, la, c_tests[i].b, lb, 5);
        int m  = myers(c_tests[i].a, la, c_tests[i].b, lb, 5);
        int d  = damerau(c_tests[i].a, la, c_tests[i].b, lb, 5);
        int h  = hybrid(c_tests[i].a, la, c_tests[i].b, lb, 5);
        int w  = wagner_fischer(c_tests[i].a, la, c_tests[i].b, lb, 5);

        int dl_expect = c_tests[i].ed;
        if (i == 13) dl_expect = 1;
        int m_ok = (m == c_tests[i].ed);
        int d_ok = (d == dl_expect);
        int w_ok = (w == c_tests[i].ed);
        int h_ok = (h == (m >= 0 && m <= 5 ? m : d));

        printf("  %s  ref=%d M=%d DL=%d H=%d WF=%d  (%s)\n",
               (m_ok && d_ok && w_ok && h_ok) ? "OK" : "FAIL",
               mr, m, d, h, w, c_tests[i].desc);
        if (m_ok && d_ok && w_ok && h_ok) pass++; else fail++;
    }
    printf("  %d/%d passed\n\n", pass, pass + fail);
    if (fail) { printf("CORRECTNESS FAILED\n"); return 1; }

    /* ── Scenario A: Random strings (worst case for fuzzy search) ── */
    static char rnd_a[N_PAIRS][MAX_LEN+1], rnd_b[N_PAIRS][MAX_LEN+1];
    static int  rnd_la[N_PAIRS], rnd_lb[N_PAIRS];
    static const char *ap[N_PAIRS], *bp[N_PAIRS];

    xs64_state = 42;
    for (int i = 0; i < N_PAIRS; i++) {
        rnd_la[i] = 4 + (int)(xs64() % 29);
        rnd_lb[i] = 4 + (int)(xs64() % 29);
        rand_str(rnd_a[i], rnd_la[i]);
        rand_str(rnd_b[i], rnd_lb[i]);
        ap[i] = rnd_a[i]; bp[i] = rnd_b[i];
    }

    printf("=== Scenario A: Random strings (max_k=%d) ===\n", MAX_K);
    printf("  %d pairs, 4-%d chars, ~0%% within edit distance\n\n", N_PAIRS, MAX_LEN);

    bench_one("Myers (corrected)",       myers,        ap, rnd_la, bp, rnd_lb, N_PAIRS, MAX_K, WARMUP, REPEATS);
    bench_one("Myers (reference, buggy)", myers_ref,    ap, rnd_la, bp, rnd_lb, N_PAIRS, MAX_K, WARMUP, REPEATS);
    bench_one("Damerau-Levenshtein",      damerau,      ap, rnd_la, bp, rnd_lb, N_PAIRS, MAX_K, WARMUP, REPEATS);
    bench_one("Hybrid (Myers + DL)",      hybrid,       ap, rnd_la, bp, rnd_lb, N_PAIRS, MAX_K, WARMUP, REPEATS);
    bench_one("Wagner-Fischer",           wagner_fischer,ap, rnd_la, bp, rnd_lb, N_PAIRS, MAX_K, WARMUP, REPEATS);
    printf("\n");

    /* ── Scenario B: Near-match strings (realistic for trigram-filtered DB) ── */
    static char near_a[N_PAIRS][MAX_LEN+1], near_b[N_PAIRS][MAX_LEN+1];
    static int  near_la[N_PAIRS], near_lb[N_PAIRS];

    xs64_state = 99;  /* different seed */
    for (int i = 0; i < N_PAIRS; i++) {
        int len = 6 + (int)(xs64() % 26);  /* 6-31 chars */
        rand_str(near_a[i], len);
        near_la[i] = len;
        /* Apply 0-3 random edits → guaranteed within ed≤3 */
        int nedits = (int)(xs64() % (MAX_K + 1));  /* 0..MAX_K */
        near_lb[i] = mutate(near_a[i], near_la[i], near_b[i], nedits);
    }

    printf("=== Scenario B: Near-match strings (max_k=%d) ===\n", MAX_K);
    printf("  %d pairs, 6-31 chars, all within edit distance %d\n\n", N_PAIRS, MAX_K);

    /* Compare each string against its own mutation (guaranteed near-match) */
    {
        volatile int sink = 0;
        struct timespec t0, t1;
        ed_fn fns[] = {myers, damerau, hybrid, wagner_fischer};
        const char *fn_names[] = {"Myers (corrected)","Damerau-Levenshtein","Hybrid (Myers + DL)","Wagner-Fischer"};

        for (int a = 0; a < 4; a++) {
            int total = 0;
            sink = 0;
            for (int w = 0; w < WARMUP; w++) {
                int i = (w * 7 + 1) % N_PAIRS;
                sink += fns[a](near_a[i], near_la[i], near_b[i], near_lb[i], MAX_K);
            }
            clock_gettime(CLOCK_MONOTONIC, &t0);
            for (int r = 0; r < REPEATS; r++) {
                for (int i = 0; i < N_PAIRS; i++) {
                    sink += fns[a](near_a[i], near_la[i], near_b[i], near_lb[i], MAX_K);
                    total++;
                }
            }
            clock_gettime(CLOCK_MONOTONIC, &t1);
            double dt = (t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) / 1e9;
            printf("  %-26s  %10.0f ops/s  %8.0f ns/op\n",
                   fn_names[a], total / dt, dt / total * 1e9);
            (void)sink;
        }
    }
    printf("\n");

    /* ── Scenario C: Stratified by length ── */
    printf("=== Scenario C: Random strings, by length (max_k=%d) ===\n", MAX_K);
    printf("  %-7s %-26s %12s %9s %8s\n", "len", "algorithm", "ops/s", "ns/op", "vs WF");

    xs64_state = 42;
    for (int l = 4; l <= MAX_LEN; l += 4) {
        static char sa[200][MAX_LEN+1], sb[200][MAX_LEN+1];
        static int  sla[200], slb[200];
        static const char *sap[200], *sbp[200];
        int n = 0;
        for (int i = 0; i < N_PAIRS && n < 50; i++) {
            /* Both strings within ±2 of target length */
            if (rnd_la[i] >= l-2 && rnd_la[i] <= l+2 &&
                rnd_lb[i] >= l-2 && rnd_lb[i] <= l+2) {
                memcpy(sa[n], rnd_a[i], rnd_la[i]+1);
                memcpy(sb[n], rnd_b[i], rnd_lb[i]+1);
                sla[n] = rnd_la[i]; slb[n] = rnd_lb[i];
                sap[n] = sa[n]; sbp[n] = sb[n];
                n++;
            }
        }
        if (n < 5) continue;

        ed_fn fns[] = {myers, damerau, hybrid, wagner_fischer};
        const char *nms[] = {"Myers","Damerau-Levenshtein","Hybrid","Wagner-Fischer"};
        double ts[4];

        for (int a = 0; a < 4; a++) {
            volatile int sink = 0;
            int tot = 0;
            struct timespec t0, t1;
            for (int w = 0; w < 30; w++)
                sink += fns[a](sap[w%n], sla[w%n], sbp[(w*3)%n], slb[(w*3)%n], MAX_K);
            clock_gettime(CLOCK_MONOTONIC, &t0);
            for (int r = 0; r < 80; r++)
                for (int i = 0; i < n; i++) {
                    int j = (i * 3 + 1) % n;
                    sink += fns[a](sap[i], sla[i], sbp[j], slb[j], MAX_K);
                    tot++;
                }
            clock_gettime(CLOCK_MONOTONIC, &t1);
            double dt = (t1.tv_sec-t0.tv_sec)+(t1.tv_nsec-t0.tv_nsec)/1e9;
            ts[a] = dt / tot * 1e9;
            (void)sink;
        }

        char lbl[8];
        snprintf(lbl, sizeof(lbl), "~%d", l);
        for (int a = 0; a < 4; a++) {
            printf("  %-7s %-26s %12.0f %9.0f %7.1fx\n",
                   (a == 0) ? lbl : "", nms[a],
                   1e9/ts[a], ts[a], ts[3]/ts[a]);
        }
        printf("\n");
    }

    return 0;
}
