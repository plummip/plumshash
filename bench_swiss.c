/*
 * swiss.c — a Swiss-table (hashbrown-style) string→id map, for an
 * apples-to-apples comparison with the plumshash intern pool.
 *
 * It follows hashbrown's structure:
 *   - ctrl[M]  1 byte per slot: 0x80 = empty, 0xFF = deleted, else the
 *              7-bit tag taken from the TOP of the hash;
 *   - slots[M] the entry: the FULL 64-bit hash, the key pointer, the key
 *              length, the id — so the candidate test is a 64-bit compare
 *              and the key bytes live in an arena beside the table;
 *   - the probe walks groups of 16 slots, one vector load of ctrl bytes
 *              per step, and only slots whose tag matches are examined;
 *   - the group index comes from the low bits of the hash and the groups
 *              are walked consecutively.
 *
 * The hash is plumshash — the SAME hash the pool uses — so the comparison
 * isolates the table and not the hash function.
 *
 * Build: clang -O3 -march=native -I. -o swiss swiss.c
 */
/*
 * bench_swiss.c — the plumshash intern pool against a Swiss table
 * (hashbrown-style), same process, same strings, same hash.
 *
 * The point of the comparison: hashbrown's algorithm IS a Swiss table, and
 * phase 2 rebuilt the intern pool into one (contiguous ctrl bytes, 16-slot
 * groups, the tag from the top of the hash, a group walk from the low bits).
 * So the two differ in what they carry, not in how they probe:
 *
 *   swiss : the FULL 64-bit hash in the slot, the key in a separate arena,
 *           a bare id out, everything lock-free.
 *   pool  : the arena LOCATION in the slot (8 B), an 8-byte prefix per
 *           string, a location HANDLE out — and a read path that never
 *           touches the table at all.
 *
 * That last one is why the pool can win the path a compiler actually runs
 * (resolve an id -> its data) and still lose the by-name path (it has to
 * build a handle, with its epoch and generation, where the map returns a
 * bare id).
 *
 * Build: clang -O3 -std=c11 -march=native -I. -o bench_swiss bench_swiss.c
 * Run:   ./bench_swiss [n] [reps]
 */
#define PLUMSHASH_IMPLEMENTATION
#include "plumshash.h"
#include "hybrid_intern.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#if defined(__ARM_NEON) || defined(__aarch64__)
#include <arm_neon.h>
#endif

#define CTRL_EMPTY   0x80u
#define CTRL_DELETED 0xFFu
#define GROUP        16

typedef struct {
    uint64_t hash;
    const char *key;
    uint32_t len;
    uint32_t id;
} Slot;

typedef struct {
    uint8_t  *ctrl;
    Slot     *slots;
    uint32_t  M;          /* slots */
    uint32_t  groups;
    uint64_t  seed;
    /* arena for the key bytes */
    char     *arena;
    size_t    arena_used;
    size_t    arena_cap;
    uint32_t  count;
} Swiss;

static double now_sec(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

static void swiss_init(Swiss *t, uint32_t expected, uint64_t seed) {
    uint64_t target = (uint64_t)expected * 4 / 3;
    uint32_t M = 1;
    while ((uint64_t)M < target) M <<= 1;
    t->M = M;
    t->groups = M / GROUP;
    t->seed = seed;
    t->ctrl = (uint8_t *)calloc(M, 1);
    t->slots = (Slot *)calloc(M, sizeof(Slot));
    t->arena_cap = (size_t)expected * 48 + 4096;
    t->arena = (char *)malloc(t->arena_cap);
    t->arena_used = 0;
    t->count = 0;
    memset(t->ctrl, CTRL_EMPTY, M);
}

/* the group scan: which slots carry our tag */
static uint32_t swiss_tag_mask(const uint8_t *c, uint8_t tag) {
#if defined(__ARM_NEON) || defined(__aarch64__)
    uint8x16_t v = vld1q_u8(c);
    uint8x16_t m = vceqq_u8(v, vdupq_n_u8(tag));
    uint8x16_t sh = vshrq_n_u8(m, 7);
    uint64_t lo = vget_lane_u64(vreinterpret_u64_u8(vget_low_u8(sh)), 0);
    uint64_t hi = vget_lane_u64(vreinterpret_u64_u8(vget_high_u8(sh)), 0);
    lo = ((lo & 0x0101010101010101ULL) * 0x0102040810204080ULL) >> 56;
    hi = ((hi & 0x0101010101010101ULL) * 0x0102040810204080ULL) >> 56;
    return (uint32_t)lo | ((uint32_t)hi << 8);
#else
    uint32_t m = 0;
    for (uint32_t i = 0; i < GROUP; i++) if (c[i] == tag) m |= 1u << i;
    return m;
#endif
}

static uint32_t swiss_empty_mask(const uint8_t *c) {
#if defined(__ARM_NEON) || defined(__aarch64__)
    uint8x16_t v = vld1q_u8(c);
    uint8x16_t m = vceqq_u8(v, vdupq_n_u8((uint8_t)CTRL_EMPTY));
    uint8x16_t sh = vshrq_n_u8(m, 7);
    uint64_t lo = vget_lane_u64(vreinterpret_u64_u8(vget_low_u8(sh)), 0);
    uint64_t hi = vget_lane_u64(vreinterpret_u64_u8(vget_high_u8(sh)), 0);
    lo = ((lo & 0x0101010101010101ULL) * 0x0102040810204080ULL) >> 56;
    hi = ((hi & 0x0101010101010101ULL) * 0x0102040810204080ULL) >> 56;
    return (uint32_t)lo | ((uint32_t)hi << 8);
#else
    uint32_t m = 0;
    for (uint32_t i = 0; i < GROUP; i++) if (c[i] == CTRL_EMPTY) m |= 1u << i;
    return m;
#endif
}

/* find or insert. Returns the id, or UINT32_MAX if the table is full. */
static uint32_t swiss_intern(Swiss *t, const char *key, uint32_t len) {
    uint64_t h = plumshash(key, len, t->seed);
    uint8_t tag = (uint8_t)(h >> 57);
    if (tag >= CTRL_EMPTY) tag = 1;          /* 0x80/0xFF are reserved */

    uint32_t probe = (uint32_t)h & (t->M - 1);
    uint32_t first_del = UINT32_MAX;
    for (uint32_t g = 0; g < t->groups; g++) {
        uint32_t group = probe & ~(uint32_t)(GROUP - 1);
        uint32_t mm = swiss_tag_mask(t->ctrl + group, tag);
        uint32_t em = swiss_empty_mask(t->ctrl + group);
        uint32_t base = group;
        if (g == 0 && (probe & (GROUP - 1))) {
            uint32_t before = (1u << (probe & (GROUP - 1))) - 1u;
            mm &= ~before; em &= ~before;
        }
        while (mm) {
            uint32_t i = __builtin_ctz(mm);
            mm &= mm - 1;
            Slot *s = &t->slots[base + i];
            if (s->hash != h) continue;
            if (s->len != len) continue;
            if (memcmp(s->key, key, len) != 0) continue;
            return s->id;
        }
        if (first_del == UINT32_MAX) {
            for (uint32_t i = 0; i < GROUP; i++) {
                if (t->ctrl[base + i] == CTRL_DELETED) { first_del = base + i; break; }
            }
        }
        if (em) {
            uint32_t slot = first_del != UINT32_MAX ? first_del
                          : base + (uint32_t)__builtin_ctz(em);
            /* copy the key into the arena */
            if (t->arena_used + len + 8 > t->arena_cap) {
                t->arena_cap = t->arena_cap * 2 + len + 64;
                t->arena = realloc(t->arena, t->arena_cap);
            }
            char *dst = t->arena + t->arena_used;
            memcpy(dst, key, len);
            dst[len] = '\0';
            t->arena_used += len + 8;
            Slot *s = &t->slots[slot];
            s->hash = h;
            s->key = dst;
            s->len = len;
            s->id = t->count++;
            t->ctrl[slot] = tag;
            return s->id;
        }
        probe = (probe + GROUP) & (t->M - 1);
    }
    return UINT32_MAX;
}


/* The plumshash pool, same strings, same hash, same process — interleaved
 * with the Swiss table so the CPU clock cannot drift between them. */
static double pool_by_handle_best = 1e30;

static void pool_run(uint32_t n, char *strings, uint32_t reps,
                     double *best_ins, double *best_hit) {
    for (int r = 0; r < reps; r++) {
        HybridPool *hp = hybrid_create(7, n, n);
        if (!hp) return;
        double t0 = now_sec();
        for (uint32_t i = 0; i < n; i++)
            hybrid_intern(hp, strings + (size_t)i * 32, 31);
        double dt = now_sec() - t0;
        if (dt < *best_ins) *best_ins = dt;

        t0 = now_sec();
        uint64_t acc = 0;
        for (uint32_t i = 0; i < n; i++)
            acc += hybrid_intern(hp, strings + (size_t)i * 32, 31).u;
        dt = now_sec() - t0;
        if (dt < *best_hit) *best_hit = dt;

        /* the by-handle read path: the handle names the arena location, so
         * the table is never touched */
        SF_Handle *hs = malloc(sizeof(SF_Handle) * n);
        for (uint32_t i = 0; i < n; i++)
            hs[i] = hybrid_intern(hp, strings + (size_t)i * 32, 31);
        t0 = now_sec();
        uint64_t acc2 = 0;
        for (uint32_t i = 0; i < n; i++)
            acc2 += (uint64_t)(uintptr_t)hybrid_str(hp, hs[i], NULL);
        dt = now_sec() - t0;
        if (dt < pool_by_handle_best) pool_by_handle_best = dt;
        if (acc2 == 0x123456789ULL) printf("x");
        free(hs);
        hybrid_destroy(hp);
    }
}

int main(int argc, char **argv) {
    uint32_t n = (uint32_t)(argc > 1 ? atoi(argv[1]) : 100000);
    int reps = (argc > 2 ? atoi(argv[2]) : 7);
    char *strings = (char *)malloc((size_t)n * 32);
    for (uint32_t i = 0; i < n; i++) {
        char *p = strings + (size_t)i * 32;
        snprintf(p, 32, "s%08u", i);
        memset(p + strlen(p), '.', 32 - strlen(p));
        p[31] = '\0';
    }

    double ps_ins = 1e30, ps_hit = 1e30, sw_ins = 1e30, sw_hit = 1e30;
    for (int r = 0; r < reps; r++) {
        /* --- swiss --- */
        {
            Swiss t; swiss_init(&t, n, 0x1234567890abcdefULL);
            double t0 = now_sec();
            for (uint32_t i = 0; i < n; i++) swiss_intern(&t, strings + (size_t)i * 32, 31);
            double dt = now_sec() - t0; if (dt < sw_ins) sw_ins = dt;
            t0 = now_sec();
            uint64_t acc = 0;
            for (uint32_t i = 0; i < n; i++) acc += swiss_intern(&t, strings + (size_t)i * 32, 31);
            dt = now_sec() - t0; if (dt < sw_hit) sw_hit = dt;
            if (acc == 0x123456789ULL) printf("x");
            free(t.ctrl); free(t.slots); free(t.arena);
        }
        /* --- plumshash pool --- */
        pool_run(n, strings, 1, &ps_ins, &ps_hit);
    }

    printf("bench case=swiss_insert     ns=%.1f n=%u\n", sw_ins * 1e9 / n, n);
    printf("bench case=swiss_lookup     ns=%.1f n=%u\n", sw_hit * 1e9 / n, n);
    printf("bench case=pool_insert      ns=%.1f n=%u\n", ps_ins * 1e9 / n, n);
    printf("bench case=pool_lookup      ns=%.1f n=%u\n", ps_hit * 1e9 / n, n);
    printf("bench case=pool_by_handle   ns=%.1f n=%u\n", pool_by_handle_best * 1e9 / n, n);
    return 0;
}
