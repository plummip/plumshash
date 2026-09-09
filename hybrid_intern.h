/*
 * hybrid_intern.h — lock-free-read, write-serialized intern pool on
 *                   PlumsHash, with SPUR-tier + e-mesh-epoch reclamation.
 * =========================================================================
 * A string-interning structure with two tiers of STORAGE and two tiers of
 * LIFETIME, grounded in the same machinery as achtc's memory model:
 *
 *   LIFETIME TIERS (SPUR):
 *     T1  stable   — intern(s) puts a string here (the DEFAULT). Its bytes
 *         live until the pool is destroyed.
 *     T0  scratch  — intern_scratch(s) puts a string here. It is auto-cleared
 *         in bulk at the next epoch boundary. Zero-copy pointers into T0 are
 *         valid only until hybrid_epoch_end().
 *
 *   EPOCHS (e-mesh):
 *     hybrid_epoch_end() invalidates every live scratch entry (gen bumped)
 *     and returns the T0 bump arena to its base in one bulk free — no
 *     per-entry work, no caller evict. Stale scratch handles resolve to NULL
 *     afterward. This is the e-mesh boundary distilled: a quiescent point
 *     where all scratch state is provably dropped.
 *
 *   SIZE TIERS (independent): strings <= SHORT_MAX_LEN go to a smaller,
 *   hotter short pool; longer strings to the long pool. Both pools carry a
 *   full T0/T1 pair.
 *
 * All string bytes live in IMMUTABLE bump-allocated arenas, so str() is
 * zero-copy: it returns a raw pointer. Reads are lock-free behind a
 * per-pool seqlock; writes are serialized by a pthread mutex. Handles are
 * NaN-boxed {pos | gen | pool | tier}.
 *
 * Use:
 *   #define PLUMSHASH_IMPLEMENTATION
 *   #include "hybrid_intern.h"
 *
 * Compile: -O3 -std=c11 -march=native -pthread
 */
#pragma once

#include <stdatomic.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <assert.h>
#include <time.h>
#include <pthread.h>
#ifdef __AVX2__
#include <immintrin.h>
#endif
#ifdef __linux__
#include <sys/random.h>   /* getrandom */
#include <unistd.h>
#endif

#include "plumshash.h"

/* ----------------------------------------------------------------------
 * TUNING
 * ---------------------------------------------------------------------- */
#ifndef SHORT_MAX_LEN
# define SHORT_MAX_LEN 48
#endif
#ifndef LONG_LOAD_NUM
# define LONG_LOAD_NUM 3
#endif
#ifndef LONG_LOAD_DEN
# define LONG_LOAD_DEN 4
#endif
#ifndef IP_AVG_STRLEN
# define IP_AVG_STRLEN 32
#endif
#ifndef IP_CHUNK_MIN
# define IP_CHUNK_MIN (256 * 1024)
#endif
#define LONG_MAX_CHUNKS 256

/* ----------------------------------------------------------------------
 * HANDLE ENCODING (NaN-boxed): {rank | gen | pool | tier}
 * ---------------------------------------------------------------------- */
#define IR_NAN          0x7FF0000000000000ULL
#define IR_RANK_SH      24
#define IR_GEN_SH       16
#define IR_POOL_SH      3
#define IR_TIER_T0      0x1ULL   /* transient (epoch-scoped) */
#define IR_TIER_T1      0x2ULL   /* stable */

typedef union { uint64_t u; void *p; } SF_Handle;
#define SF_NULL ((SF_Handle){ .u = 0 })

static inline SF_Handle _encode_handle(uint32_t rank, uint8_t gen, uint16_t pool_id, uint64_t tier) {
    return (SF_Handle){ .u = IR_NAN |
        ((uint64_t)rank    << IR_RANK_SH) |
        ((uint64_t)gen     << IR_GEN_SH)  |
        ((uint64_t)pool_id << IR_POOL_SH) |
        tier };
}
static inline uint32_t _handle_rank(SF_Handle h) { return (uint32_t)((h.u >> IR_RANK_SH) & 0x0FFFFFFFull); }
static inline uint8_t  _handle_gen (SF_Handle h) { return (uint8_t) ((h.u >> IR_GEN_SH)  & 0xFFull); }
static inline uint16_t _handle_pool(SF_Handle h) { return (uint16_t)((h.u >> IR_POOL_SH) & 0x1FFFull); }
static inline uint64_t _handle_tier(SF_Handle h) { return h.u & 0x3ull; }
static inline bool     _handle_valid(SF_Handle h) { return (h.u & 0x7FF0000000000000ULL) == IR_NAN; }

/* ----------------------------------------------------------------------
 * Write lock + seqlock
 * ---------------------------------------------------------------------- */
static inline void _lock(pthread_mutex_t *m) { pthread_mutex_lock(m); }
static inline void _unlock(pthread_mutex_t *m) { pthread_mutex_unlock(m); }

static inline void _seq_begin(_Atomic uint32_t *s) { atomic_fetch_add_explicit(s, 1, memory_order_release); }
static inline void _seq_end  (_Atomic uint32_t *s) { atomic_fetch_add_explicit(s, 1, memory_order_release); }

static uint64_t _entropy(void) {
    uint64_t s = 0;
#ifdef __linux__
    if (getrandom(&s, sizeof(s), 0) != (ssize_t)sizeof(s))
#endif
    { s = (uint64_t)(uintptr_t)&s; s ^= (uint64_t)time(NULL) * 0x9E3779B97F4A7C15ULL; }
    return s;
}

/* ----------------------------------------------------------------------
 * IMMUTABLE BUMP ARENA
 * ---------------------------------------------------------------------- */
typedef struct DataChunk {
    struct DataChunk  *next;
    uint32_t           cap;
    _Atomic uint32_t   bump;
    uint8_t            _pad[48];
    uint8_t            data[];
} DataChunk;
_Static_assert(sizeof(DataChunk) == 64, "DataChunk must be 64 bytes");

typedef struct {
    DataChunk *chunks[LONG_MAX_CHUNKS];
    uint16_t   n_chunks;
    uint8_t    cur_chunk;
} Arena;

static DataChunk *_arena_chunk_new(uint32_t cap) {
    cap = (cap + 31) & ~31;
    DataChunk *c = (DataChunk *)malloc(sizeof(DataChunk) + cap);
    assert(c);
    c->next = NULL; c->cap = cap;
    atomic_store_explicit(&c->bump, 0, memory_order_relaxed);
    memset(c->_pad, 0, sizeof(c->_pad));
    return c;
}

static void _arena_init(Arena *a, uint32_t first_cap) {
    a->chunks[0] = _arena_chunk_new(first_cap);
    a->n_chunks = 1;
    a->cur_chunk = 0;
}

static void _arena_free(Arena *a) {
    for (uint16_t i = 0; i < a->n_chunks; i++) free(a->chunks[i]);
}

static void _arena_alloc(Arena *a, uint32_t len, uint8_t *cid_out, uint32_t *off_out) {
    uint32_t need = (len + 31) & ~31;
    if (!need) need = 32;
    uint8_t cid = a->cur_chunk;
    DataChunk *cur = a->chunks[cid];
    uint32_t off = atomic_fetch_add_explicit(&cur->bump, need, memory_order_relaxed);
    if (off + need > cur->cap) {
        assert(a->n_chunks < LONG_MAX_CHUNKS);
        uint16_t new_id = a->n_chunks++;
        uint32_t new_cap = cur->cap * 2;
        if (new_cap < IP_CHUNK_MIN) new_cap = IP_CHUNK_MIN;
        if (new_cap < need * 4) new_cap = need * 4;
        a->chunks[new_id] = _arena_chunk_new(new_cap);
        a->cur_chunk = (uint8_t)new_id;
        cid = (uint8_t)new_id;
        off = atomic_fetch_add_explicit(&a->chunks[cid]->bump, need, memory_order_relaxed);
    }
    *cid_out = cid; *off_out = off;
}

/* ----------------------------------------------------------------------
 * POOL — slot-indexed headers + T0/T1 arenas + fingerprint
 * ---------------------------------------------------------------------- */
typedef enum : uint8_t { LK_FREE = 0, LK_OWNED = 1 } EntryKind;
enum { TIER_T0 = 0, TIER_T1 = 1 };

typedef struct __attribute__((aligned(4))) {
    uint32_t hash_gen;          /* [31:8] = 24-bit hash, [7:0] = monotonic gen */
    uint16_t len;
    uint8_t  kind;              /* LK_FREE / LK_OWNED */
    uint8_t  chunk_id;
    uint32_t offset;
    uint8_t  tier;              /* TIER_T0 / TIER_T1 — which arena */
    uint8_t  _pad[19];
} Entry;
_Static_assert(sizeof(Entry) == 32, "Entry must be 32 bytes");

typedef struct {
    Entry            *headers;
    Arena             t0;       /* scratch — bulk-freed at epoch_end */
    Arena             t1;       /* stable — freed at destroy */
    uint32_t          M;
    uint16_t          pool_id;
    uint64_t          seed;
    _Atomic uint32_t  n_entries;
    _Atomic uint32_t  n_collisions;
    _Atomic uint32_t  seq;
    uint32_t          max_entries;
    pthread_mutex_t   lock;
} Pool;

static inline const uint8_t *_entry_data(const Pool *p, const Entry *eh) {
    const Arena *a = eh->tier == TIER_T1 ? &p->t1 : &p->t0;
    return a->chunks[eh->chunk_id]->data + eh->offset;
}

static inline bool _entry_matches(const Pool *p, const Entry *eh, const char *s, uint32_t len) {
    const uint8_t *ed = _entry_data(p, eh);
#ifdef __AVX2__
    uint32_t i = 0;
    for (; i + 32 <= len; i += 32) {
        __m256i a = _mm256_loadu_si256((const __m256i *)(ed + i));
        __m256i b = _mm256_loadu_si256((const __m256i *)(s + i));
        if (_mm256_movemask_epi8(_mm256_cmpeq_epi8(a, b)) != 0xFFFFFFFF) return false;
    }
    if (i < len) {
        uint32_t rem = len - i;
        __m256i a = _mm256_loadu_si256((const __m256i *)(ed + i));
        __m256i b = _mm256_loadu_si256((const __m256i *)(s + i));
        uint32_t mask = _mm256_movemask_epi8(_mm256_cmpeq_epi8(a, b));
        uint32_t expected = (1u << rem) - 1u;
        if ((mask & expected) != expected) return false;
    }
    return true;
#else
    return memcmp(ed, s, len) == 0;
#endif
}

/* publish word 1 (offset+tier) then word 0 (hash_gen|len|kind|chunk_id) LAST
 * with release, so a lock-free reader that acquires word 0 sees word 1. */
static inline void _store_header(Entry *dst, const Entry *src) {
    const uint64_t *s = (const uint64_t *)src;
    uint64_t *d = (uint64_t *)dst;
    __atomic_store_n(d + 1, s[1], __ATOMIC_RELAXED);
    __atomic_store_n(d,     s[0], __ATOMIC_RELEASE);
}

static Pool *pool_create(uint16_t pool_id, uint32_t expected, uint64_t seed) {
    if (expected < 64) expected = 64;
    uint32_t M = 1;
    while (M < (uint32_t)(expected * LONG_LOAD_DEN / LONG_LOAD_NUM)) M <<= 1;

    Pool *p = (Pool *)calloc(1, sizeof(Pool));
    p->headers = (Entry *)aligned_alloc(64, M * sizeof(Entry));
    assert(p->headers);
    p->M = M;
    p->pool_id = pool_id;
    p->seed = seed;
    p->max_entries = (uint32_t)(M * LONG_LOAD_NUM / LONG_LOAD_DEN);
    pthread_mutex_init(&p->lock, NULL);
    memset(p->headers, 0, M * sizeof(Entry));

    uint32_t cap = expected * IP_AVG_STRLEN;
    if (cap < IP_CHUNK_MIN) cap = IP_CHUNK_MIN;
    _arena_init(&p->t0, cap);
    _arena_init(&p->t1, cap);
    return p;
}

static void pool_destroy(Pool *p) {
    _arena_free(&p->t0);
    _arena_free(&p->t1);
    free(p->headers); free(p);
}

static inline uint64_t _entry_tier_bits(const Entry *eh) { return eh->tier == TIER_T1 ? IR_TIER_T1 : IR_TIER_T0; }

static SF_Handle pool_intern(Pool *p, const char *s, uint32_t len, uint8_t tier) {
    uint64_t h = plumshash(s, len, p->seed);
    uint32_t h32 = (uint32_t)h;
    uint32_t pos = h32 & (p->M - 1);
    Arena *a = tier == TIER_T1 ? &p->t1 : &p->t0;

    _lock(&p->lock);
    _seq_begin(&p->seq);
    SF_Handle out = SF_NULL;
    for (uint32_t probe = 0; probe < p->M; probe++) {
        uint32_t p2 = (pos + probe) & (p->M - 1);
        Entry *eh = &p->headers[p2];
        uint64_t hdr = __atomic_load_n((uint64_t *)eh, __ATOMIC_RELAXED);

        if (((uint8_t)(hdr >> 48)) == LK_FREE) {
            uint8_t cid; uint32_t off;
            _arena_alloc(a, len, &cid, &off);
            uint8_t *dst = a->chunks[cid]->data + off;
            memcpy(dst, s, len);
            uint32_t padded = (len + 31) & ~31;
            if (padded == 0) padded = 32;
            if (padded > len) memset(dst + len, 0, padded - len);

            uint8_t gen = (uint8_t)(hdr & 0xFF) + 1;
            if (gen == 0) gen = 1;

            Entry ne = {0};
            ne.hash_gen = ((uint32_t)h32 << 8) | gen;
            ne.len = (uint16_t)len;
            ne.kind = LK_OWNED;
            ne.chunk_id = cid;
            ne.offset = off;
            ne.tier = tier;
            _store_header(eh, &ne);
            atomic_fetch_add_explicit(&p->n_entries, 1, memory_order_relaxed);
            out = _encode_handle(p2, gen, p->pool_id, tier == TIER_T1 ? IR_TIER_T1 : IR_TIER_T0);
            break;
        }

        uint32_t hg = (uint32_t)hdr;
        uint16_t elen = (uint16_t)(hdr >> 32);
        if (eh->tier == tier && ((hg ^ ((uint32_t)h32 << 8)) < 0x100u) && elen == len
            && _entry_matches(p, eh, s, len)) {
            out = _encode_handle(p2, (uint8_t)hg, p->pool_id, _entry_tier_bits(eh));
            break;
        }
        atomic_fetch_add_explicit(&p->n_collisions, 1, memory_order_relaxed);
    }
    _seq_end(&p->seq);
    _unlock(&p->lock);
    return out;
}

static void pool_evict(Pool *p, SF_Handle h) {
    if (!_handle_valid(h) || _handle_pool(h) != p->pool_id) return;
    uint32_t pos = _handle_rank(h);
    uint8_t  gen = _handle_gen(h);
    _lock(&p->lock);
    _seq_begin(&p->seq);
    Entry *eh = &p->headers[pos];
    uint64_t hdr = __atomic_load_n((uint64_t *)eh, __ATOMIC_ACQUIRE);
    if (((uint8_t)(hdr >> 48)) == LK_OWNED && (uint8_t)hdr == gen) {
        uint8_t ng = (uint8_t)(hdr & 0xFF) + 1; if (ng == 0) ng = 1;
        uint64_t nhdr = (hdr & ~0xFFull) | ng;   /* bump gen */
        nhdr &= ~(0xFFull << 48);                /* kind = LK_FREE */
        __atomic_store_n((uint64_t *)eh, nhdr, __ATOMIC_RELEASE);
        atomic_fetch_sub_explicit(&p->n_entries, 1, memory_order_relaxed);
    }
    _seq_end(&p->seq);
    _unlock(&p->lock);
}

static const char *pool_str(Pool *p, SF_Handle h, uint32_t *len_out) {
    if (!_handle_valid(h) || _handle_pool(h) != p->pool_id) return NULL;
    uint32_t pos = _handle_rank(h);
    uint8_t  gen = _handle_gen(h);
    Entry *eh = &p->headers[pos];
    const char *data;
    uint32_t s0, s1;
    do {
        do { s0 = atomic_load_explicit(&p->seq, memory_order_acquire); }
        while (s0 & 1);
        uint64_t hdr = __atomic_load_n((uint64_t *)eh, __ATOMIC_ACQUIRE);
        if ((uint8_t)hdr != gen) return NULL;
        data = (const char *)_entry_data(p, eh);
        if (len_out) *len_out = (uint16_t)(hdr >> 32);
        s1 = atomic_load_explicit(&p->seq, memory_order_acquire);
    } while (s0 != s1);
    return data;
}

/* e-mesh epoch boundary: auto-clear the scratch (T0) tier. Every live T0
 * entry's slot is invalidated (gen bumped) and the T0 arena is bulk-freed
 * and reset — no per-entry work, no caller evict. Stale scratch handles
 * resolve to NULL afterward. Returns the number of scratch entries cleared
 * (0 means the caller had no live scratch at the boundary). */
static uint32_t pool_epoch_end(Pool *p) {
    uint32_t cleared = 0;
    _lock(&p->lock);
    _seq_begin(&p->seq);
    for (uint32_t i = 0; i < p->M; i++) {
        Entry *eh = &p->headers[i];
        uint64_t hdr = __atomic_load_n((uint64_t *)eh, __ATOMIC_ACQUIRE);
        if (((uint8_t)(hdr >> 48)) == LK_OWNED && eh->tier == TIER_T0) {
            uint8_t ng = (uint8_t)(hdr & 0xFF) + 1; if (ng == 0) ng = 1;
            uint64_t nhdr = (hdr & ~0xFFull) | ng;
            nhdr &= ~(0xFFull << 48);   /* kind = LK_FREE */
            __atomic_store_n((uint64_t *)eh, nhdr, __ATOMIC_RELEASE);
            atomic_fetch_sub_explicit(&p->n_entries, 1, memory_order_relaxed);
            cleared++;
        }
    }
    _arena_free(&p->t0);
    uint32_t cap = p->max_entries * IP_AVG_STRLEN;
    if (cap < IP_CHUNK_MIN) cap = IP_CHUNK_MIN;
    _arena_init(&p->t0, cap);
    _seq_end(&p->seq);
    _unlock(&p->lock);
    return cleared;
}

/* ----------------------------------------------------------------------
 * HYBRID POOL — short tier + long tier, each with T0/T1
 * ---------------------------------------------------------------------- */
typedef struct {
    Pool    *short_pool;
    Pool    *long_pool;
    uint16_t base_pool_id;
} HybridPool;

static HybridPool *hybrid_create(uint16_t base_pool_id,
                                 uint32_t expected_short,
                                 uint32_t expected_long) {
    uint64_t seed = _entropy();
    HybridPool *hp = (HybridPool *)calloc(1, sizeof(HybridPool));
    assert(hp);
    hp->base_pool_id = base_pool_id;
    hp->short_pool = pool_create((uint16_t)(base_pool_id * 2 + 1), expected_short, seed);
    hp->long_pool  = pool_create((uint16_t)(base_pool_id * 2),     expected_long,  seed);
    return hp;
}

static void hybrid_destroy(HybridPool *hp) {
    if (hp->short_pool) pool_destroy(hp->short_pool);
    if (hp->long_pool)  pool_destroy(hp->long_pool);
    free(hp);
}

static Pool *_tier_pool(HybridPool *hp, SF_Handle h) {
    return (_handle_pool(h) & 1) ? hp->short_pool : hp->long_pool;
}

static SF_Handle hybrid_intern(HybridPool *hp, const char *s, uint32_t len) {       /* T1 stable (default) */
    Pool *p = (len <= SHORT_MAX_LEN) ? hp->short_pool : hp->long_pool;
    return pool_intern(p, s, len, TIER_T1);
}

static SF_Handle hybrid_intern_scratch(HybridPool *hp, const char *s, uint32_t len) {/* T0 transient */
    Pool *p = (len <= SHORT_MAX_LEN) ? hp->short_pool : hp->long_pool;
    return pool_intern(p, s, len, TIER_T0);
}

static void hybrid_evict(HybridPool *hp, SF_Handle h) {
    if (!_handle_valid(h)) return;
    pool_evict(_tier_pool(hp, h), h);
}

static const char *hybrid_str(HybridPool *hp, SF_Handle h, uint32_t *len_out) {
    if (!_handle_valid(h)) return NULL;
    return pool_str(_tier_pool(hp, h), h, len_out);
}

/* auto-clear both pools' scratch tiers; returns the total cleared */
static uint32_t hybrid_epoch_end(HybridPool *hp) {
    return pool_epoch_end(hp->short_pool) + pool_epoch_end(hp->long_pool);
}

/* ----------------------------------------------------------------------
 * STATS
 * ---------------------------------------------------------------------- */
typedef struct {
    uint32_t short_entries, short_max;
    uint32_t long_entries, long_max, long_collisions;
    double   short_load, long_load;
} HybridStats;

static HybridStats hybrid_stats(HybridPool *hp) {
    HybridStats s = {0};
    s.short_entries = atomic_load_explicit(&hp->short_pool->n_entries, memory_order_relaxed);
    s.short_max = hp->short_pool->M;
    s.long_entries = atomic_load_explicit(&hp->long_pool->n_entries, memory_order_relaxed);
    s.long_max = hp->long_pool->M;
    s.long_collisions = atomic_load_explicit(&hp->long_pool->n_collisions, memory_order_relaxed);
    s.short_load = (double)s.short_entries / s.short_max;
    s.long_load = (double)s.long_entries / s.long_max;
    return s;
}

static void hybrid_print_stats(HybridPool *hp) {
    HybridStats s = hybrid_stats(hp);
    fprintf(stderr,
        "[HybridPool id=%u]\n"
        "  Short pool: %u / %u slots (%.1f%%)\n"
        "  Long pool : %u / %u slots (%.1f%%), collisions=%u\n",
        hp->base_pool_id,
        s.short_entries, s.short_max, s.short_load * 100.0,
        s.long_entries, s.long_max, s.long_load * 100.0,
        s.long_collisions);
}

static inline void _hybrid_cleanup(HybridPool **hp) { if (*hp) { hybrid_destroy(*hp); *hp = NULL; } }
#define HYBRID_SCOPED(name, id, short_n, long_n) \
    HybridPool *name __attribute__((cleanup(_hybrid_cleanup))) = hybrid_create((id), (short_n), (long_n))
