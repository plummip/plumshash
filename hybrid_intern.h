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
#if defined(__AVX2__) || defined(__SSE2__)
#include <immintrin.h>
#endif
#if defined(__ARM_NEON) || defined(__aarch64__)
#include <arm_neon.h>
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
/* Arena allocation granularity. Every interned string carries an 8-byte
 * prefix (length + reserved), so the granularity is the floor on the
 * per-string overhead: 16 bytes keeps it at ~8-24 B/string instead of the
 * 32 B/string a 32-byte granularity would cost. The AVX2 compare never
 * reads past the string, so the stored side needs no 32-byte slack. */
#ifndef IP_ARENA_ALIGN
# define IP_ARENA_ALIGN 16
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
    cap = cap > UINT32_MAX - (IP_ARENA_ALIGN - 1u) ? UINT32_MAX
         : ((cap + (IP_ARENA_ALIGN - 1u)) & ~(uint32_t)(IP_ARENA_ALIGN - 1u));
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
    uint32_t need = (len + (IP_ARENA_ALIGN - 1u)) & ~(uint32_t)(IP_ARENA_ALIGN - 1u);
    if (!need) need = IP_ARENA_ALIGN;
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
 * POOL — ctrl-byte table + 8-byte slots + T0/T1 arenas
 * ---------------------------------------------------------------------- */
/* The table is split in two:
 *
 *   ctrl[M]   one byte per slot. 0x00 = never used (ends a probe chain),
 *             0x80 = deleted (tombstone — the chain runs PAST it), any
 *             other value = the 7-bit tag of the entry in that slot.
 *   slots[M]  eight bytes per slot: where the bytes live, which arena, a
 *             wider fingerprint, and the slot generation.
 *
 * The ctrl array is 32x denser than the old 32-byte header, so a probe
 * checks 32 slots per cache line and mostly stays in cache; the slot array
 * is touched only when a tag actually matches.
 */
#define IP_CTRL_EMPTY    0x00u
#define IP_CTRL_DELETED  0x80u

/* The tombstone model (and the tombstone counters in HybridStats) arrived
 * with hybrid_intern 1.1. Tests that want to run against an older header
 * can test this macro to skip the counters. */
#define HYBRID_INTERN_HAS_TOMBSTONES 1
enum { TIER_T0 = 0, TIER_T1 = 1 };

/* Slot layout (64 bits):
 *   bits  0..31  arena offset
 *   bits 32..39  arena chunk id
 *   bit  40      tier (T0 scratch / T1 stable)
 *   bits 41..55  fingerprint (15 bits)
 *   bits 56..63  slot generation
 *
 * The length is NOT in the slot: it lives in the arena prefix, in the same
 * cache line as the first bytes of the string. */
typedef uint64_t IpSlot;

#define IP_SLOT_OFFSET(u) ((uint32_t)((u) & 0xFFFFFFFFu))
#define IP_SLOT_CHUNK(u)  ((uint8_t)(((u) >> 32) & 0xFFu))
#define IP_SLOT_TIER(u)   ((uint8_t)(((u) >> 40) & 0x1u))
#define IP_SLOT_FP(u)     ((uint16_t)(((u) >> 41) & 0x7FFFu))
#define IP_SLOT_GEN(u)    ((uint8_t)(((u) >> 56) & 0xFFu))
#define IP_SLOT_MAKE(off, chunk, tier, fp, gen)                  \
    ((uint64_t)(off) | ((uint64_t)(chunk) << 32) |               \
     ((uint64_t)(tier) << 40) | ((uint64_t)(fp) << 41) |         \
     ((uint64_t)(gen) << 56))

/* Tag and fingerprint come from disjoint parts of the hash, and both are
 * disjoint from the index bits. The old header took its fingerprint from
 * hash bits 8..31, which OVERLAPPED the index (bits 0..20) — so the cheap
 * pre-memcmp reject could not discriminate same-bucket candidates at all.
 * The index now uses the full 64-bit hash instead of a 32-bit truncation. */
#define IP_HASH_TAG(h) ((uint8_t)(((h) >> 57) & 0x7Fu))
#define IP_HASH_FP(h)  ((uint16_t)(((h) >> 41) & 0x7FFFu))

/* Probe group size: one vector load of ctrl bytes per step. */
#if defined(__AVX2__)
# define IP_GROUP 32
#elif defined(__SSE2__)
# define IP_GROUP 16
#else
# define IP_GROUP 16
#endif

/* Every interned string is stored as an 8-byte prefix followed by its
 * bytes: [len:32][reserved:32][bytes...]. The prefix shares a cache line
 * with the first bytes of the string, so a lookup reads the length and the
 * data from one line. */
typedef struct { uint32_t len; uint32_t reserved; } IpPrefix;

typedef struct {
    uint8_t           *ctrl;     /* 1 byte per slot */
    IpSlot           *slots;    /* 8 bytes per slot */
    Arena             t0;       /* scratch — bulk-freed at epoch_end */
    Arena             t1;       /* stable — freed at destroy */
    uint32_t          M;
    uint16_t          pool_id;
    uint64_t          seed;
    _Atomic uint32_t  n_entries;
    _Atomic uint32_t  n_tombs;
    _Atomic uint32_t  n_collisions;
    _Atomic uint32_t  seq;
    uint32_t          max_entries;
    pthread_mutex_t   lock;
} Pool;

/* Where a slot's string lives: the arena prefix, then the bytes. */
static inline const uint8_t *_slot_base(const Pool *p, uint64_t sl) {
    const Arena *a = IP_SLOT_TIER(sl) == TIER_T1 ? &p->t1 : &p->t0;
    return a->chunks[IP_SLOT_CHUNK(sl)]->data + IP_SLOT_OFFSET(sl);
}

static inline bool _bytes_equal(const uint8_t *ed, const char *s, uint32_t len) {
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

static inline uint32_t _ctz32(uint32_t v) {
#if defined(__GNUC__) || defined(__clang__)
    return (uint32_t)__builtin_ctz(v);
#else
    uint32_t n = 0; while (!(v & 1u)) { v >>= 1; n++; } return n;
#endif
}

#if defined(__ARM_NEON) || defined(__aarch64__)
/* NEON has no movemask: the compare result is 0xFF per matching lane, so
 * the mask is built by shifting each lane down to one bit and gathering
 * the bits with the usual multiply trick. */
static inline uint32_t _neon_mask16(uint8x16_t m) {
    uint8x16_t s = vshrq_n_u8(m, 7);            /* 0xFF lanes -> 0x01 lanes */
    uint64_t lo = vget_lane_u64(vreinterpret_u64_u8(vget_low_u8(s)), 0);
    uint64_t hi = vget_lane_u64(vreinterpret_u64_u8(vget_high_u8(s)), 0);
    lo = ((lo & 0x0101010101010101ULL) * 0x0102040810204080ULL) >> 56;
    hi = ((hi & 0x0101010101010101ULL) * 0x0102040810204080ULL) >> 56;
    return (uint32_t)lo | ((uint32_t)hi << 8);
}
#endif

/* Scan one group of ctrl bytes: which slots carry our tag, which are
 * deleted, which are still empty. One vector load, three compares. */
static inline void _ctrl_scan(const uint8_t *c, uint8_t tag,
                              uint32_t *match, uint32_t *deleted,
                              uint32_t *empty) {
#if defined(__AVX2__)
    __m256i v  = _mm256_loadu_si256((const __m256i *)c);
    __m256i tv = _mm256_set1_epi8((char)tag);
    __m256i dv = _mm256_set1_epi8((char)IP_CTRL_DELETED);
    __m256i ev = _mm256_setzero_si256();
    *match   = (uint32_t)_mm256_movemask_epi8(_mm256_cmpeq_epi8(v, tv));
    *deleted = (uint32_t)_mm256_movemask_epi8(_mm256_cmpeq_epi8(v, dv));
    *empty   = (uint32_t)_mm256_movemask_epi8(_mm256_cmpeq_epi8(v, ev));
#elif defined(__SSE2__)
    __m128i v  = _mm_loadu_si128((const __m128i *)c);
    __m128i tv = _mm_set1_epi8((char)tag);
    __m128i dv = _mm_set1_epi8((char)IP_CTRL_DELETED);
    __m128i ev = _mm_setzero_si128();
    *match   = (uint32_t)_mm_movemask_epi8(_mm_cmpeq_epi8(v, tv));
    *deleted = (uint32_t)_mm_movemask_epi8(_mm_cmpeq_epi8(v, dv));
    *empty   = (uint32_t)_mm_movemask_epi8(_mm_cmpeq_epi8(v, ev));
#elif defined(__ARM_NEON) || defined(__aarch64__)
    uint8x16_t v  = vld1q_u8(c);
    *match   = _neon_mask16(vceqq_u8(v, vdupq_n_u8(tag)));
    *deleted = _neon_mask16(vceqq_u8(v, vdupq_n_u8((uint8_t)IP_CTRL_DELETED)));
    *empty   = _neon_mask16(vceqq_u8(v, vdupq_n_u8(0)));
#else
    uint32_t m = 0, d = 0, e = 0;
    for (uint32_t i = 0; i < IP_GROUP; i++) {
        uint32_t bit = 1u << i;
        if (c[i] == tag)                 m |= bit;
        else if (c[i] == IP_CTRL_DELETED) d |= bit;
        else if (c[i] == IP_CTRL_EMPTY)   e |= bit;
    }
    *match = m; *deleted = d; *empty = e;
#endif
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
    size_t ctrl_bytes = ((size_t)M + 63u) & ~(size_t)63u;   /* aligned_alloc needs a multiple */
    p->ctrl = (uint8_t *)aligned_alloc(64, ctrl_bytes);
    p->slots = (IpSlot *)aligned_alloc(64, ((size_t)M * sizeof(IpSlot) + 63u) & ~(size_t)63u);
    if (!p->ctrl || !p->slots) { free(p->ctrl); free(p->slots); free(p); return NULL; }
    p->M = M;
    p->pool_id = pool_id;
    p->seed = seed;
    p->max_entries = (uint32_t)(M * LONG_LOAD_NUM / LONG_LOAD_DEN);
    pthread_mutex_init(&p->lock, NULL);
    memset(p->ctrl, 0, ctrl_bytes);

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
    free(p->ctrl); free(p->slots); free(p);
}

/* The tier a handle carries for a given entry tier. */
static inline uint64_t _tier_bits(uint8_t tier) { return tier == TIER_T1 ? IR_TIER_T1 : IR_TIER_T0; }

/* Walk the probe chain for (h, len, s).
 *
 * Sets match_slot (and match_gen) when an existing entry is found, else
 * insert_slot — the slot where a new entry goes. The chain is scanned a
 * group at a time: one vector load of ctrl bytes per step, and the slot
 * array is touched only for slots whose tag matches. */
static void _pool_probe(Pool *p, uint64_t h, uint32_t len,
                        const char *s, uint8_t tier,
                        uint32_t *match_slot, uint8_t *match_gen,
                        uint32_t *insert_slot) {
    const uint32_t M = p->M;
    const uint32_t home = (uint32_t)h & (M - 1);
    uint8_t  tag = IP_HASH_TAG(h);
    if (!tag) tag = 1;                       /* 0 means "never used" */
    const uint16_t fp = IP_HASH_FP(h);

    const uint32_t gmask = IP_GROUP - 1;
    uint32_t group = home & ~gmask;
    const uint32_t groups = M / IP_GROUP;
    uint32_t first_del = UINT32_MAX;

    *match_slot = UINT32_MAX;
    *match_gen = 0;
    *insert_slot = UINT32_MAX;

    for (uint32_t g = 0; g < groups; g++) {
        uint32_t mm, dm, em;
        _ctrl_scan(p->ctrl + group, tag, &mm, &dm, &em);
        if (g == 0 && (home & gmask)) {
            /* slots before `home` in the first group belong to other chains */
            uint32_t before = (1u << (home & gmask)) - 1u;
            mm &= ~before; dm &= ~before; em &= ~before;
        }
        while (mm) {
            uint32_t i = _ctz32(mm);
            mm &= mm - 1;
            uint32_t slot = group + i;
            uint64_t sl = __atomic_load_n(&p->slots[slot], __ATOMIC_RELAXED);
            atomic_fetch_add_explicit(&p->n_collisions, 1, memory_order_relaxed);
            if (IP_SLOT_FP(sl) != fp) continue;
            if (IP_SLOT_TIER(sl) != tier) continue;
            const uint8_t *base = _slot_base(p, sl);
            uint32_t elen = ((const IpPrefix *)base)->len;
            if (elen != len) continue;
            if (!_bytes_equal(base + sizeof(IpPrefix), s, len)) continue;
            *match_slot = slot;
            *match_gen = IP_SLOT_GEN(sl);
            return;
        }
        if (first_del == UINT32_MAX && dm) {
            uint32_t i = _ctz32(dm);
            first_del = group + i;
        }
        if (em) {
            /* The chain ends in this group. Insert at the first tombstone we
             * passed (a tombstone is a usable slot), else at the first empty
             * slot. */
            uint32_t i = _ctz32(em);
            *insert_slot = (first_del != UINT32_MAX) ? first_del : (group + i);
            return;
        }
        group = (group + IP_GROUP) & (M - 1);
    }
    /* The whole table was walked: no empty slot, but maybe a tombstone. */
    *insert_slot = first_del;
}

static SF_Handle pool_intern(Pool *p, const char *s, uint32_t len, uint8_t tier) {
    /* The arena prefix (8 bytes) rides along with every string, so the
     * usable length is 8 less than the arena limit. */
    if (len > HYBRID_INTERN_MAX_LEN - 8u || (len != 0 && s == NULL)) return SF_NULL;
    uint64_t h = plumshash(s, len, p->seed);
    Arena *a = tier == TIER_T1 ? &p->t1 : &p->t0;

    _lock(&p->lock);
    _seq_begin(&p->seq);
    SF_Handle out = SF_NULL;
    uint32_t match_slot, insert_slot;
    uint8_t  match_gen;
    _pool_probe(p, h, len, s, tier, &match_slot, &match_gen, &insert_slot);

    if (match_slot != UINT32_MAX) {
        out = _encode_handle(match_slot, match_gen, p->pool_id, _tier_bits(tier));
    } else if (insert_slot != UINT32_MAX) {
        uint32_t total = (len + 8u + (IP_ARENA_ALIGN - 1u)) & ~(uint32_t)(IP_ARENA_ALIGN - 1u);
        uint8_t cid; uint32_t off;
        if (_arena_alloc(a, len + 8u, &cid, &off)) {
            uint8_t *base = a->chunks[cid]->data + off;
            IpPrefix *pfx = (IpPrefix *)base;
            uint8_t *dst = base + sizeof(IpPrefix);
            uint32_t padded = total - (uint32_t)sizeof(IpPrefix);
            pfx->len = len;
            pfx->reserved = 0;
            if (len) memcpy(dst, s, len);
            if (padded > len) memset(dst + len, 0, padded - len);

            uint64_t old = __atomic_load_n(&p->slots[insert_slot], __ATOMIC_RELAXED);
            uint8_t gen = (uint8_t)(IP_SLOT_GEN(old) + 1);
            if (!gen) gen = 1;
            uint64_t sl = IP_SLOT_MAKE(off, cid, tier, IP_HASH_FP(h), gen);
            __atomic_store_n(&p->slots[insert_slot], sl, __ATOMIC_RELAXED);

            uint8_t tag = IP_HASH_TAG(h);
            if (!tag) tag = 1;
            uint8_t old_ctrl = __atomic_load_n(&p->ctrl[insert_slot], __ATOMIC_RELAXED);
            /* Publish the ctrl byte LAST: a reader that sees OCCUPIED also
             * sees the slot and the string bytes. */
            __atomic_store_n(&p->ctrl[insert_slot], tag, __ATOMIC_RELEASE);
            atomic_fetch_add_explicit(&p->n_entries, 1, memory_order_relaxed);
            if (old_ctrl == IP_CTRL_DELETED)
                atomic_fetch_sub_explicit(&p->n_tombs, 1, memory_order_relaxed);
            out = _encode_handle(insert_slot, gen, p->pool_id, _tier_bits(tier));
        }
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
    uint8_t tier = (want_tier == IR_TIER_T1) ? TIER_T1 : TIER_T0;
    _lock(&p->lock);
    _seq_begin(&p->seq);
    uint8_t ctrl = __atomic_load_n(&p->ctrl[pos], __ATOMIC_ACQUIRE);
    if (ctrl != IP_CTRL_EMPTY && ctrl != IP_CTRL_DELETED) {
        uint64_t sl = __atomic_load_n(&p->slots[pos], __ATOMIC_RELAXED);
        if (IP_SLOT_GEN(sl) == gen && IP_SLOT_TIER(sl) == tier) {
            uint8_t ng = (uint8_t)(IP_SLOT_GEN(sl) + 1); if (ng == 0) ng = 1;
            uint64_t nsl = (sl & ~(0xFFull << 56)) | ((uint64_t)ng << 56);
            __atomic_store_n(&p->slots[pos], nsl, __ATOMIC_RELAXED);
            /* Tombstone: the slot stays part of the chain but no longer
             * holds an entry. */
            __atomic_store_n(&p->ctrl[pos], IP_CTRL_DELETED, __ATOMIC_RELEASE);
            atomic_fetch_sub_explicit(&p->n_entries, 1, memory_order_relaxed);
            atomic_fetch_add_explicit(&p->n_tombs, 1, memory_order_relaxed);
        }
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
    uint8_t tier = (want_tier == IR_TIER_T1) ? TIER_T1 : TIER_T0;
    const char *data;
    uint32_t len = 0;
    uint32_t s0, s1;
    do {
        do { s0 = atomic_load_explicit(&p->seq, memory_order_acquire); }
        while (s0 & 1);
        uint8_t ctrl = __atomic_load_n(&p->ctrl[pos], __ATOMIC_ACQUIRE);
        if (ctrl == IP_CTRL_EMPTY || ctrl == IP_CTRL_DELETED) return NULL;
        uint64_t sl = __atomic_load_n(&p->slots[pos], __ATOMIC_RELAXED);
        if (IP_SLOT_GEN(sl) != gen || IP_SLOT_TIER(sl) != tier) return NULL;
        const uint8_t *base = _slot_base(p, sl);
        len = ((const IpPrefix *)base)->len;
        data = (const char *)(base + sizeof(IpPrefix));
        s1 = atomic_load_explicit(&p->seq, memory_order_acquire);
    } while (s0 != s1);
    if (len_out) *len_out = len;
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
        uint8_t ctrl = __atomic_load_n(&p->ctrl[i], __ATOMIC_ACQUIRE);
        if (ctrl == IP_CTRL_EMPTY || ctrl == IP_CTRL_DELETED) continue;
        uint64_t sl = __atomic_load_n(&p->slots[i], __ATOMIC_RELAXED);
        if (IP_SLOT_TIER(sl) != TIER_T0) continue;
        uint8_t ng = (uint8_t)(IP_SLOT_GEN(sl) + 1); if (ng == 0) ng = 1;
        uint64_t nsl = (sl & ~(0xFFull << 56)) | ((uint64_t)ng << 56);
        __atomic_store_n(&p->slots[i], nsl, __ATOMIC_RELAXED);
        __atomic_store_n(&p->ctrl[i], IP_CTRL_DELETED, __ATOMIC_RELEASE);
        atomic_fetch_sub_explicit(&p->n_entries, 1, memory_order_relaxed);
        atomic_fetch_add_explicit(&p->n_tombs, 1, memory_order_relaxed);
        cleared++;
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
    uint32_t short_collisions;        /* probe-chain collisions, short tier */
    uint32_t short_tombs, long_tombs; /* deleted slots awaiting reuse */
    double   short_load, long_load;
} HybridStats;

static HybridStats hybrid_stats(HybridPool *hp) {
    HybridStats s = {0};
    s.short_entries = atomic_load_explicit(&hp->short_pool->n_entries, memory_order_relaxed);
    s.short_max = hp->short_pool->M;
    s.short_collisions = atomic_load_explicit(&hp->short_pool->n_collisions, memory_order_relaxed);
    s.short_tombs = atomic_load_explicit(&hp->short_pool->n_tombs, memory_order_relaxed);
    s.long_entries = atomic_load_explicit(&hp->long_pool->n_entries, memory_order_relaxed);
    s.long_max = hp->long_pool->M;
    s.long_tombs = atomic_load_explicit(&hp->long_pool->n_tombs, memory_order_relaxed);
    s.long_collisions = atomic_load_explicit(&hp->long_pool->n_collisions, memory_order_relaxed);
    s.short_load = (double)s.short_entries / s.short_max;
    s.long_load = (double)s.long_entries / s.long_max;
    return s;
}

static void hybrid_print_stats(HybridPool *hp) {
    HybridStats s = hybrid_stats(hp);
    fprintf(stderr,
        "[HybridPool id=%u]\n"
        "  Short pool: %u / %u slots (%.1f%%), tombstones=%u, collisions=%u\n"
        "  Long pool : %u / %u slots (%.1f%%), tombstones=%u, collisions=%u\n",
        hp->base_pool_id,
        s.short_entries, s.short_max, s.short_load * 100.0, s.short_tombs, s.short_collisions,
        s.long_entries, s.long_max, s.long_load * 100.0, s.long_tombs,
        s.long_collisions);
}

static inline void _hybrid_cleanup(HybridPool **hp) { if (*hp) { hybrid_destroy(*hp); *hp = NULL; } }
#define HYBRID_SCOPED(name, id, short_n, long_n) \
    HybridPool *name __attribute__((cleanup(_hybrid_cleanup))) = hybrid_create((id), (short_n), (long_n))
