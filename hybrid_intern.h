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
 * VERSION
 * ---------------------------------------------------------------------- */
/* hybrid_intern.h first shipped in PlumsHash v1.2.0. It carries its own
 * version because it evolves on its own schedule — plumshash.h's
 * PLUMSHASH_VERSION tracks the hash itself. */
#define HYBRID_INTERN_VERSION_MAJOR 1
#define HYBRID_INTERN_VERSION_MINOR 0
#define HYBRID_INTERN_VERSION_PATCH 0
#define HYBRID_INTERN_VERSION "1.0.0"

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
#ifndef HYBRID_INTERN_MAX_LEN
/* Leave headroom for 32-byte arena rounding without uint32 overflow. */
# define HYBRID_INTERN_MAX_LEN (UINT32_MAX - 31u)
#endif
#define LONG_MAX_CHUNKS 256
/* The handle pool-id field is 13 bits and derived ids are 2*base and 2*base+1. */
#define HYBRID_INTERN_MAX_POOL_ID 4095u
#ifndef HYBRID_INTERN_MAX_ENTRIES
/* Upper bound on the per-pool entry-count hint. A pool costs 8 bytes per
 * table slot plus expected*IP_AVG_STRLEN bytes per tier arena, so a mistyped
 * hint must not be able to ask for unbounded memory. */
# define HYBRID_INTERN_MAX_ENTRIES (1u << 20)
#endif

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
    cap = cap > UINT32_MAX - 31u ? UINT32_MAX : ((cap + 31u) & ~31u);
    if ((size_t)cap > SIZE_MAX - sizeof(DataChunk)) return NULL;
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

static bool _arena_alloc(Arena *a, uint32_t len, uint8_t *cid_out, uint32_t *off_out) {
    if (len > HYBRID_INTERN_MAX_LEN) return false;
    uint32_t need = (len + 31u) & ~31u;
    if (!need) need = 32;
    uint8_t cid = a->cur_chunk;
    DataChunk *cur = a->chunks[cid];
    uint32_t off = atomic_load_explicit(&cur->bump, memory_order_relaxed);
    if (off > cur->cap || need > cur->cap - off) {
        if (a->n_chunks >= LONG_MAX_CHUNKS) return false;
        uint64_t target = (uint64_t)need * 4u;
        if (target > UINT32_MAX) target = need;
        uint32_t new_cap = cur->cap;
        if (new_cap < target) {
            uint64_t doubled = (uint64_t)new_cap * 2u;
            new_cap = doubled > UINT32_MAX ? UINT32_MAX : (uint32_t)doubled;
            if (new_cap < target) new_cap = (uint32_t)target;
        }
        DataChunk *new_chunk = _arena_chunk_new(new_cap);
        if (!new_chunk) return false;
        uint16_t new_id = a->n_chunks++;
        a->chunks[new_id] = new_chunk;
        a->cur_chunk = (uint8_t)new_id;
        cid = (uint8_t)new_id;
        off = atomic_fetch_add_explicit(&new_chunk->bump, need,
                                        memory_order_relaxed);
        if (off > new_chunk->cap || need > new_chunk->cap - off) return false;
    } else {
        off = atomic_fetch_add_explicit(&cur->bump, need,
                                        memory_order_relaxed);
    }
    *cid_out = cid; *off_out = off;
    return true;
}

/* ----------------------------------------------------------------------
 * POOL — slot-indexed headers + T0/T1 arenas + fingerprint
 * ---------------------------------------------------------------------- */
typedef enum : uint8_t { LK_FREE = 0, LK_OWNED = 1 } EntryKind;
enum { TIER_T0 = 0, TIER_T1 = 1 };

typedef struct __attribute__((aligned(4))) {
    uint32_t hash_gen;          /* [31:8] = 24-bit hash, [7:0] = monotonic gen */
    uint8_t  kind;              /* LK_FREE / LK_OWNED */
    uint8_t  chunk_id;
    uint8_t  tier;              /* TIER_T0 / TIER_T1 — which arena */
    uint8_t  _pad0;
    uint32_t len;               /* full uint32 length */
    uint32_t offset;
    uint8_t  _pad1[16];
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

/* Decode entry location from the two atomically published header words.
 * Word 0 = hash/gen + kind/chunk/tier; word 1 = len/offset. */
static inline const uint8_t *_entry_data_snapshot(const Pool *p,
                                                   uint64_t hdr0,
                                                   uint64_t hdr1) {
    uint8_t tier = (uint8_t)(hdr0 >> 48);
    uint8_t chunk = (uint8_t)(hdr0 >> 40);
    uint32_t offset = (uint32_t)(hdr1 >> 32);
    const Arena *a = tier == TIER_T1 ? &p->t1 : &p->t0;
    return a->chunks[chunk]->data + offset;
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
        /* A 32-byte vector load would read past a short caller buffer.
         * The stored side is padded, but the input side is not. */
        if (memcmp(ed + i, s + i, rem) != 0) return false;
    }
    return true;
#else
    return memcmp(ed, s, len) == 0;
#endif
}

/* Publish word 1 (len/offset) then word 0 (hash/gen/kind/chunk/tier) LAST
 * with release, so a lock-free reader that acquires word 0 sees word 1. */
static inline void _store_header(Entry *dst, const Entry *src) {
    const uint64_t *s = (const uint64_t *)src;
    uint64_t *d = (uint64_t *)dst;
    __atomic_store_n(d + 1, s[1], __ATOMIC_RELAXED);
    __atomic_store_n(d,     s[0], __ATOMIC_RELEASE);
}

static Pool *pool_create(uint16_t pool_id, uint32_t expected, uint64_t seed) {
    if (expected > HYBRID_INTERN_MAX_ENTRIES) return NULL;
    if (expected < 64) expected = 64;
    /* 64-bit math: the hint can be large enough to overflow a uint32 product. */
    uint64_t target = (uint64_t)expected * LONG_LOAD_DEN / LONG_LOAD_NUM;
    uint32_t M = 1;
    while ((uint64_t)M < target) M <<= 1;

    Pool *p = (Pool *)calloc(1, sizeof(Pool));
    if (!p) return NULL;
    p->headers = (Entry *)aligned_alloc(64, (size_t)M * sizeof(Entry));
    if (!p->headers) { free(p); return NULL; }
    p->M = M;
    p->pool_id = pool_id;
    p->seed = seed;
    p->max_entries = (uint32_t)(M * LONG_LOAD_NUM / LONG_LOAD_DEN);
    pthread_mutex_init(&p->lock, NULL);
    memset(p->headers, 0, M * sizeof(Entry));

    uint64_t cap64 = (uint64_t)expected * IP_AVG_STRLEN;
    uint32_t cap = cap64 > UINT32_MAX ? UINT32_MAX : (uint32_t)cap64;
    if (cap < IP_CHUNK_MIN) cap = IP_CHUNK_MIN;
    _arena_init(&p->t0, cap);
    _arena_init(&p->t1, cap);
    return p;
}

static void pool_destroy(Pool *p) {
    if (!p) return;
    _arena_free(&p->t0);
    _arena_free(&p->t1);
    free(p->headers); free(p);
}

static inline uint64_t _entry_tier_bits(const Entry *eh) { return eh->tier == TIER_T1 ? IR_TIER_T1 : IR_TIER_T0; }

static SF_Handle pool_intern(Pool *p, const char *s, uint32_t len, uint8_t tier) {
    if (len > HYBRID_INTERN_MAX_LEN || (len != 0 && s == NULL)) return SF_NULL;
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

        if (((uint8_t)(hdr >> 32)) == LK_FREE) {
            uint8_t cid; uint32_t off;
            if (!_arena_alloc(a, len, &cid, &off)) break;
            uint8_t *dst = a->chunks[cid]->data + off;
            if (len) memcpy(dst, s, len);
            uint32_t padded = (len + 31) & ~31;
            if (padded == 0) padded = 32;
            if (padded > len) memset(dst + len, 0, padded - len);

            uint8_t gen = (uint8_t)(hdr & 0xFF) + 1;
            if (gen == 0) gen = 1;

            Entry ne = {0};
            ne.hash_gen = ((uint32_t)h32 << 8) | gen;
            ne.len = len;
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
        uint64_t hdr1 = __atomic_load_n((uint64_t *)eh + 1, __ATOMIC_RELAXED);
        uint32_t elen = (uint32_t)hdr1;
        uint8_t etier = (uint8_t)(hdr >> 48);
        if (etier == tier && ((hg ^ ((uint32_t)h32 << 8)) < 0x100u) && elen == len
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
    if (pos >= p->M) return;
    uint8_t  gen = _handle_gen(h);
    uint8_t  want_tier = _handle_tier(h);
    if (want_tier != IR_TIER_T0 && want_tier != IR_TIER_T1) return;
    _lock(&p->lock);
    _seq_begin(&p->seq);
    Entry *eh = &p->headers[pos];
    uint64_t hdr = __atomic_load_n((uint64_t *)eh, __ATOMIC_ACQUIRE);
    uint8_t entry_tier = (uint8_t)(hdr >> 48);
    uint8_t expected_entry_tier = want_tier == IR_TIER_T1 ? TIER_T1 : TIER_T0;
    if (((uint8_t)(hdr >> 32)) == LK_OWNED && (uint8_t)hdr == gen &&
        entry_tier == expected_entry_tier) {
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
    if (pos >= p->M) return NULL;
    uint8_t  gen = _handle_gen(h);
    uint8_t  want_tier = _handle_tier(h);
    if (want_tier != IR_TIER_T0 && want_tier != IR_TIER_T1) return NULL;
    Entry *eh = &p->headers[pos];
    const char *data;
    uint32_t s0, s1;
    do {
        do { s0 = atomic_load_explicit(&p->seq, memory_order_acquire); }
        while (s0 & 1);
        uint64_t hdr = __atomic_load_n((uint64_t *)eh, __ATOMIC_ACQUIRE);
        uint64_t hdr1 = __atomic_load_n((uint64_t *)eh + 1, __ATOMIC_RELAXED);
        uint8_t entry_tier = (uint8_t)(hdr >> 48);
        if (((uint8_t)(hdr >> 32)) != LK_OWNED || (uint8_t)hdr != gen ||
            entry_tier != (want_tier == IR_TIER_T1 ? TIER_T1 : TIER_T0))
            return NULL;
        data = (const char *)_entry_data_snapshot(p, hdr, hdr1);
        if (len_out) *len_out = (uint32_t)hdr1;
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
        if (((uint8_t)(hdr >> 32)) == LK_OWNED &&
            (uint8_t)(hdr >> 48) == TIER_T0) {
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

static void hybrid_destroy(HybridPool *hp);

static HybridPool *hybrid_create(uint16_t base_pool_id,
                                 uint32_t expected_short,
                                 uint32_t expected_long) {
    if (base_pool_id > HYBRID_INTERN_MAX_POOL_ID) return NULL;
    uint64_t seed = _entropy();
    HybridPool *hp = (HybridPool *)calloc(1, sizeof(HybridPool));
    if (!hp) return NULL;
    hp->base_pool_id = base_pool_id;
    hp->short_pool = pool_create((uint16_t)(base_pool_id * 2 + 1), expected_short, seed);
    hp->long_pool  = pool_create((uint16_t)(base_pool_id * 2),     expected_long,  seed);
    if (!hp->short_pool || !hp->long_pool) {
        hybrid_destroy(hp);
        return NULL;
    }
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
