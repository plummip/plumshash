/*
 * hybrid_intern.h — lock-free-read, write-serialized hybrid intern pool
 *                   built on PlumsHash.
 * =========================================================================
 * A string-interning structure with two tiers:
 *   - Short pool: strings <= SHORT_MAX_LEN stored inline in 64-byte slots
 *     (no heap allocation on the hot path).
 *   - Long pool : longer strings in a bump-allocated chunk arena, indexed
 *     by a lock-serialized bitmap + rank structure.
 *
 * Handles are NaN-boxed 64-bit values: {rank(28) | gen(8) | pool(13) | tier}.
 * They are valid NaNs, so they can live inside a double in a tagged-union
 * language runtime without a separate discriminant word.
 *
 * CONCURRENCY MODEL
 * -----------------
 *   - Reads  (str lookup) are LOCK-FREE: a single acquire load of the
 *     slot/header's publish word orders the data payload.
 *   - Writes (intern / evict) are serialized by a per-pool spinlock.
 *
 * Why not fully lock-free? A fixed-size inline slot holds a 48-byte string,
 * which cannot be published atomically with a single CAS. The original
 * (lock-free CAS) design had two publication races: it published the
 * slot/bit *before* writing the payload, so a concurrent reader could see
 * a live slot with torn data, and a concurrent intern of the same string
 * produced a duplicate handle. The seqlock on the long pool's bitmap did
 * not protect the header because the bit was claimed outside its window.
 * This rewrite serializes writers (spinlock) and publishes the payload
 * *last* behind a release store; readers acquire that store, so they see a
 * consistent entry. Interning is rare relative to lookup, so the locked
 * write path costs almost nothing.
 *
 * THREAD-SAFETY & LIFETIME
 * ------------------------
 *   str() COPIES the string into a caller buffer (never returns a raw pool
 *   pointer). A per-pool seqlock makes each copy a consistent snapshot, so
 *   an evict+re-intern racing a lookup can only yield the intact old string
 *   (gen still matched) or 0 (stale) — never torn data. Handles are
 *   gen-tagged and monotonic; a stale handle resolves to length 0. This
 *   removes the evict-during-lookup TOCTOU that raw-pointer reads had.
 *   Strings themselves are immutable in the long pool (bump allocator never
 *   frees); the short pool's inline bytes are re-copied under the seqlock.
 *
 * PORTABILITY
 * -----------
 *   - No AVX2 requirement: the vectorized compare is used only under
 *     __AVX2__ (x86-64 -mavx2); everywhere else (incl. AArch64) memcmp is
 *     used. The x86 AVX2 path is safe because long-pool payloads are
 *     padded to 32-byte multiples; the short pool always uses memcmp
 *     (its 48-byte inline buffer is not 32-byte padded, so a masked AVX2
 *     tail would over-read).
 *   - Handles are NaN-boxed; this is portable to every AArch64/x86-64
 *     compiler that preserves IEEE-754 NaN payloads (all of them in
 *     practice). The payload bits (rank/gen/pool) never touch bits 63:52.
 *
 * Use:
 *   #define PLUMSHASH_IMPLEMENTATION   // in exactly one .c file
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

#include "plumshash.h"   /* uint64_t plumshash(const void*, size_t, uint64_t) */

/* ----------------------------------------------------------------------
 * TUNING
 * ---------------------------------------------------------------------- */
#ifndef SHORT_MAX_LEN
# define SHORT_MAX_LEN 48        /* bytes stored inline */
#endif
#ifndef SHORT_LOAD_FACTOR
# define SHORT_LOAD_FACTOR 0.75f
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
 * HANDLE ENCODING (NaN-boxed)
 * ---------------------------------------------------------------------- */
#define IR_TIER_T2      0x3ULL
#define IR_NAN          0x7FF0000000000000ULL
#define IR_RANK_SH      24
#define IR_GEN_SH       16
#define IR_POOL_SH      3

typedef union { uint64_t u; void *p; } SF_Handle;
#define SF_NULL ((SF_Handle){ .u = 0 })

static inline SF_Handle _encode_handle(uint32_t rank, uint8_t gen, uint16_t pool_id) {
    return (SF_Handle){ .u = IR_NAN |
        ((uint64_t)rank    << IR_RANK_SH) |
        ((uint64_t)gen     << IR_GEN_SH)  |
        ((uint64_t)pool_id << IR_POOL_SH) |
        IR_TIER_T2 };
}
static inline uint32_t _handle_rank(SF_Handle h) { return (uint32_t)((h.u >> IR_RANK_SH) & 0x0FFFFFFFull); }
static inline uint8_t  _handle_gen (SF_Handle h) { return (uint8_t) ((h.u >> IR_GEN_SH)  & 0xFFull); }
static inline uint16_t _handle_pool(SF_Handle h) { return (uint16_t)((h.u >> IR_POOL_SH) & 0x1FFFull); }
static inline bool     _handle_valid(SF_Handle h) { return (h.u & 0x7FF0000000000000ULL) == IR_NAN; }

/* ----------------------------------------------------------------------
 * Write lock (writers only). A pthread mutex rather than a spinlock: under
 * contention the kernel deschedules waiters instead of burning CPU, which
 * avoids the thundering-herd that a test-and-set spinlock hits on shared
 * or preempted cores. Reads stay lock-free and never touch this lock.
 * ---------------------------------------------------------------------- */
static inline void _lock(pthread_mutex_t *m) { pthread_mutex_lock(m); }
static inline void _unlock(pthread_mutex_t *m) { pthread_mutex_unlock(m); }

/* Seqlock (reader/writer): writers are already serialized by the pool
 * mutex; they bump seq odd before mutating and even after. A reader copies
 * out only across a stable (even, unchanged) seq, so it can never observe a
 * torn entry — this closes the evict-during-lookup TOCTOU that raw-pointer
 * reads had. */
static inline void _seq_begin(_Atomic uint32_t *s) { atomic_fetch_add_explicit(s, 1, memory_order_release); }
static inline void _seq_end  (_Atomic uint32_t *s) { atomic_fetch_add_explicit(s, 1, memory_order_release); }

/* ----------------------------------------------------------------------
 * Entropy (best-effort seed; PlumsHash is safe with seed==0 anyway)
 * ---------------------------------------------------------------------- */
static uint64_t _entropy(void) {
    uint64_t s = 0;
#ifdef __linux__
    if (getrandom(&s, sizeof(s), 0) != (ssize_t)sizeof(s))
#endif
    {
        /* fallback: ASLR address + wall clock, mixed */
        s = (uint64_t)(uintptr_t)&s;
        s ^= (uint64_t)time(NULL) * 0x9E3779B97F4A7C15ULL;
    }
    return s;
}

/* ----------------------------------------------------------------------
 * SHORT POOL — 64-byte inline slots
 * ---------------------------------------------------------------------- */
typedef struct __attribute__((aligned(64))) {
    _Atomic uint64_t hash_gen;  /* [63:8] = 32-bit hash, [7:0] = gen (monotonic) */
    _Atomic uint32_t len;
    _Atomic uint8_t  live;      /* 0 = empty, 1 = occupied; published LAST */
    uint8_t          _pad[3];
    char             data[SHORT_MAX_LEN];
} ShortSlot;

_Static_assert(sizeof(ShortSlot) == 64, "ShortSlot must be 64 bytes");

typedef struct {
    ShortSlot        *slots;
    uint32_t          M;        /* power of two */
    uint32_t          max_entries;
    uint16_t          pool_id;  /* odd */
    uint64_t          seed;
    _Atomic uint32_t  n_entries;
    _Atomic uint32_t  seq;      /* seqlock: odd during a write, even when stable */
    pthread_mutex_t   lock;
} ShortPool;

static ShortPool *short_pool_create(uint16_t pool_id, uint32_t expected, uint64_t seed) {
    if (expected < 64) expected = 64;
    uint32_t M = 1;
    while (M < (uint32_t)(expected / SHORT_LOAD_FACTOR)) M <<= 1;
    ShortPool *sp = (ShortPool *)calloc(1, sizeof(ShortPool));
    sp->slots = (ShortSlot *)aligned_alloc(64, M * sizeof(ShortSlot));
    assert(sp->slots);
    sp->M = M;
    sp->max_entries = (uint32_t)(M * SHORT_LOAD_FACTOR);
    sp->pool_id = pool_id | 1;
    sp->seed = seed;
    pthread_mutex_init(&sp->lock, NULL);
    memset(sp->slots, 0, M * sizeof(ShortSlot));
    return sp;
}

static void short_pool_destroy(ShortPool *sp) { free(sp->slots); free(sp); }

static inline bool _short_slot_match(const ShortSlot *slot, const char *s, uint32_t len) {
    /* memcmp on <=48 bytes: the compiler vectorizes this; the AVX2 path is
     * unsafe here because the 48-byte buffer is not 32-byte padded. */
    return memcmp(slot->data, s, len) == 0;
}

static SF_Handle short_intern(ShortPool *sp, const char *s, uint32_t len) {
    if (len > SHORT_MAX_LEN) return SF_NULL;

    uint64_t h = plumshash(s, len, sp->seed);
    uint32_t h32 = (uint32_t)h;
    uint32_t pos = h32 & (sp->M - 1);

    _lock(&sp->lock);
    _seq_begin(&sp->seq);
    SF_Handle out = SF_NULL;
    for (uint32_t probe = 0; probe < sp->M; probe++) {
        uint32_t p = (pos + probe) & (sp->M - 1);
        ShortSlot *slot = &sp->slots[p];

        if (atomic_load_explicit(&slot->live, memory_order_relaxed)) {
            uint64_t hg = atomic_load_explicit(&slot->hash_gen, memory_order_relaxed);
            if ((hg >> 8) == h32
                && atomic_load_explicit(&slot->len, memory_order_relaxed) == len
                && _short_slot_match(slot, s, len)) {
                out = _encode_handle(p, (uint8_t)hg, sp->pool_id);
                break;
            }
            continue;  /* occupied by a different string: probe */
        }

        /* empty slot: write payload, then publish 'live' LAST */
        uint8_t gen = (uint8_t)(atomic_load_explicit(&slot->hash_gen, memory_order_relaxed) & 0xFF) + 1;
        if (gen == 0) gen = 1;
        atomic_store_explicit(&slot->len, len, memory_order_relaxed);
        memcpy(slot->data, s, len);
        if (len < SHORT_MAX_LEN) memset(slot->data + len, 0, SHORT_MAX_LEN - len);
        atomic_store_explicit(&slot->hash_gen, ((uint64_t)h32 << 8) | gen, memory_order_relaxed);
        atomic_store_explicit(&slot->live, 1, memory_order_release);  /* publish */
        atomic_fetch_add_explicit(&sp->n_entries, 1, memory_order_relaxed);
        out = _encode_handle(p, gen, sp->pool_id);
        break;
    }
    _seq_end(&sp->seq);
    _unlock(&sp->lock);
    return out;  /* SF_NULL if full */
}

static void short_evict(ShortPool *sp, SF_Handle h) {
    if (!_handle_valid(h) || _handle_pool(h) != sp->pool_id) return;
    uint32_t rank = _handle_rank(h);
    uint8_t  gen  = _handle_gen(h);
    _lock(&sp->lock);
    _seq_begin(&sp->seq);
    ShortSlot *slot = &sp->slots[rank];
    if (atomic_load_explicit(&slot->live, memory_order_relaxed)
        && (uint8_t)(atomic_load_explicit(&slot->hash_gen, memory_order_relaxed) & 0xFF) == gen) {
        uint8_t ng = gen + 1; if (ng == 0) ng = 1;
        atomic_fetch_and_explicit(&slot->hash_gen, ~0xFFull, memory_order_relaxed);
        atomic_fetch_or_explicit(&slot->hash_gen, ng, memory_order_relaxed);   /* bump gen */
        atomic_store_explicit(&slot->live, 0, memory_order_release);          /* publish */
        atomic_fetch_sub_explicit(&sp->n_entries, 1, memory_order_relaxed);
    }
    _seq_end(&sp->seq);
    _unlock(&sp->lock);
}

static uint32_t short_str(ShortPool *sp, SF_Handle h, char *buf, uint32_t bufcap) {
    if (!_handle_valid(h) || _handle_pool(h) != sp->pool_id) return 0;
    uint32_t rank = _handle_rank(h);
    uint8_t  gen  = _handle_gen(h);
    ShortSlot *slot = &sp->slots[rank];
    uint32_t s0, s1, len;
    do {
        do { s0 = atomic_load_explicit(&sp->seq, memory_order_acquire); }
        while (s0 & 1);   /* writer in progress: retry */
        uint64_t hg = atomic_load_explicit(&slot->hash_gen, memory_order_acquire);
        if ((uint8_t)hg != gen) return 0;   /* stale handle */
        len = atomic_load_explicit(&slot->len, memory_order_relaxed);
        if (len > bufcap) return 0;
        memcpy(buf, slot->data, len);
        s1 = atomic_load_explicit(&sp->seq, memory_order_acquire);
    } while (s0 != s1);
    return len;
}

/* ----------------------------------------------------------------------
 * LONG POOL — slot-indexed headers + chunk arena
 * ----------------------------------------------------------------------
 * Each slot has a fixed header at headers[pos]; occupancy is encoded in the
 * header's `kind` byte. The original design used a bitmap + "rank = popcount
 * of bits before pos" compact index, which is unsound for arbitrary-order
 * insertion: inserting at a lower position after a higher one would need to
 * shift every later header, and the insert path never did — so a lower
 * insert silently overwrote a higher entry. Fixed slot indexing is trivially
 * correct; the bitmap only bought cache-friendly probing, which the header's
 * own occupancy byte provides anyway.
 */
typedef struct DataChunk {
    struct DataChunk  *next;
    uint32_t           cap;
    _Atomic uint32_t   bump;
    uint8_t            _pad[48];   /* 8+4+4+48 = 64, so data[] lands 64B-aligned */
    uint8_t            data[];
} DataChunk;

_Static_assert(sizeof(DataChunk) == 64, "DataChunk must be 64 bytes");

typedef enum : uint8_t { LK_FREE = 0, LK_OWNED = 1 } LongKind;

typedef struct __attribute__((aligned(4))) {
    uint32_t hash_gen;          /* [31:8] = 24-bit hash, [7:0] = monotonic gen */
    uint16_t len;
    uint8_t  kind;              /* LK_FREE (0) = empty, LK_OWNED (1) = live */
    uint8_t  chunk_id;
    union { uint32_t offset; uintptr_t _ext; } u;
    uint8_t  _pad[16];
} LongEntry;

_Static_assert(sizeof(LongEntry) == 32, "LongEntry must be 32 bytes");

typedef struct {
    LongEntry       *headers;   /* M entries, indexed by slot position */
    DataChunk       *chunks[LONG_MAX_CHUNKS];
    uint32_t         M;
    uint16_t         pool_id;   /* even */
    uint16_t         n_chunks;
    uint8_t          cur_chunk;
    uint64_t         seed;
    _Atomic uint32_t n_entries;
    _Atomic uint32_t n_collisions;
    _Atomic uint32_t seq;       /* seqlock: odd during a write, even when stable */
    uint32_t         max_entries;
    pthread_mutex_t  lock;
} LongPool;

static DataChunk *_long_chunk_new(uint32_t cap) {
    cap = (cap + 31) & ~31;
    DataChunk *c = (DataChunk *)malloc(sizeof(DataChunk) + cap);
    assert(c);
    c->next = NULL; c->cap = cap;
    atomic_store_explicit(&c->bump, 0, memory_order_relaxed);
    memset(c->_pad, 0, sizeof(c->_pad));
    return c;
}

/* writer-side only (called under the pool lock) */
static void _long_chunk_alloc(LongPool *p, uint32_t len, uint8_t *cid_out, uint32_t *off_out) {
    uint32_t need = (len + 31) & ~31;
    if (!need) need = 32;
    uint8_t cid = p->cur_chunk;
    DataChunk *cur = p->chunks[cid];
    uint32_t off = atomic_fetch_add_explicit(&cur->bump, need, memory_order_relaxed);
    if (off + need > cur->cap) {
        assert(p->n_chunks < LONG_MAX_CHUNKS);
        uint16_t new_id = p->n_chunks++;
        uint32_t new_cap = cur->cap * 2;
        if (new_cap < IP_CHUNK_MIN) new_cap = IP_CHUNK_MIN;
        if (new_cap < need * 4) new_cap = need * 4;
        p->chunks[new_id] = _long_chunk_new(new_cap);
        p->cur_chunk = (uint8_t)new_id;
        cid = (uint8_t)new_id;
        off = atomic_fetch_add_explicit(&p->chunks[cid]->bump, need, memory_order_relaxed);
    }
    *cid_out = cid; *off_out = off;
}

static inline const uint8_t *_long_entry_data(const LongPool *p, const LongEntry *eh) {
    return p->chunks[eh->chunk_id]->data + eh->u.offset;
}

static inline bool _long_entry_matches(const LongPool *p, const LongEntry *eh,
                                       const char *s, uint32_t len) {
    const uint8_t *ed = _long_entry_data(p, eh);
#ifdef __AVX2__
    /* safe: long-pool payloads are padded to 32-byte multiples */
    uint32_t i = 0;
    for (; i + 32 <= len; i += 32) {
        __m256i a = _mm256_loadu_si256((const __m256i *)(ed + i));
        __m256i b = _mm256_loadu_si256((const __m256i *)(s + i));
        if (_mm256_movemask_epi8(_mm256_cmpeq_epi8(a, b)) != 0xFFFFFFFF)
            return false;
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

/* publish header payload (word 1 = offset) then word 0 (hash_gen|len|kind|
 * chunk_id) LAST with release. A lock-free reader acquires word 0 and thus
 * sees the offset. __atomic_* builtins are used because LongEntry is a
 * plain struct (not _Atomic-qualified); they are the standard GCC/clang
 * mechanism for atomics on non-_Atomic storage. */
static inline void _long_store_header(LongEntry *dst, const LongEntry *src) {
    const uint64_t *s = (const uint64_t *)src;
    uint64_t *d = (uint64_t *)dst;
    __atomic_store_n(d + 1, s[1], __ATOMIC_RELAXED);  /* u.offset */
    __atomic_store_n(d,     s[0], __ATOMIC_RELEASE);  /* hash_gen|len|kind|chunk_id */
}

static LongPool *long_pool_create(uint16_t pool_id, uint32_t expected, uint64_t seed) {
    if (expected < 64) expected = 64;
    uint32_t M = 1;
    while (M < (uint32_t)(expected * LONG_LOAD_DEN / LONG_LOAD_NUM)) M <<= 1;

    LongPool *p = (LongPool *)calloc(1, sizeof(LongPool));
    p->headers = (LongEntry *)aligned_alloc(64, M * sizeof(LongEntry));
    assert(p->headers);
    p->M = M;
    p->pool_id = pool_id & ~1;
    p->seed = seed;
    p->max_entries = (uint32_t)(M * LONG_LOAD_NUM / LONG_LOAD_DEN);
    pthread_mutex_init(&p->lock, NULL);
    memset(p->headers, 0, M * sizeof(LongEntry));

    uint32_t first_cap = expected * IP_AVG_STRLEN;
    if (first_cap < IP_CHUNK_MIN) first_cap = IP_CHUNK_MIN;
    p->chunks[0] = _long_chunk_new(first_cap);
    p->n_chunks = 1;
    p->cur_chunk = 0;
    return p;
}

static void long_pool_destroy(LongPool *p) {
    for (uint16_t i = 0; i < p->n_chunks; i++) free(p->chunks[i]);
    free(p->headers); free(p);
}

static SF_Handle long_intern(LongPool *p, const char *s, uint32_t len) {
    uint64_t h = plumshash(s, len, p->seed);
    uint32_t h32 = (uint32_t)h;
    uint32_t pos = h32 & (p->M - 1);

    _lock(&p->lock);
    _seq_begin(&p->seq);
    SF_Handle out = SF_NULL;
    for (uint32_t probe = 0; probe < p->M; probe++) {
        uint32_t p2 = (pos + probe) & (p->M - 1);
        LongEntry *eh = &p->headers[p2];
        uint64_t hdr = __atomic_load_n((uint64_t *)eh, __ATOMIC_RELAXED);  /* under lock */

        if (((uint8_t)(hdr >> 48)) == LK_FREE) {   /* empty slot */
            uint8_t cid; uint32_t off;
            _long_chunk_alloc(p, len, &cid, &off);
            uint8_t *dst = p->chunks[cid]->data + off;
            memcpy(dst, s, len);
            uint32_t padded = (len + 31) & ~31;
            if (padded > len) memset(dst + len, 0, padded - len);

            uint8_t gen = (uint8_t)(hdr & 0xFF) + 1;   /* carry forward monotonic gen */
            if (gen == 0) gen = 1;

            LongEntry ne = {0};
            ne.hash_gen = ((uint32_t)h32 << 8) | gen;
            ne.len = (uint16_t)len;
            ne.kind = LK_OWNED;
            ne.chunk_id = cid;
            ne.u.offset = off;
            _long_store_header(eh, &ne);
            atomic_fetch_add_explicit(&p->n_entries, 1, memory_order_relaxed);
            out = _encode_handle(p2, gen, p->pool_id);
            break;
        }

        /* occupied: dedup check */
        uint32_t hg = (uint32_t)hdr;
        uint16_t elen = (uint16_t)(hdr >> 32);
        if (((hg ^ ((uint32_t)h32 << 8)) < 0x100u) && elen == len
            && _long_entry_matches(p, eh, s, len)) {
            out = _encode_handle(p2, (uint8_t)hg, p->pool_id);
            break;
        }
        atomic_fetch_add_explicit(&p->n_collisions, 1, memory_order_relaxed);
    }
    _seq_end(&p->seq);
    _unlock(&p->lock);
    return out;  /* SF_NULL if full */
}

static void long_evict(LongPool *p, SF_Handle h) {
    if (!_handle_valid(h) || _handle_pool(h) != p->pool_id) return;
    uint32_t pos = _handle_rank(h);
    uint8_t  gen = _handle_gen(h);
    _lock(&p->lock);
    _seq_begin(&p->seq);
    LongEntry *eh = &p->headers[pos];
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

static uint32_t long_str(LongPool *p, SF_Handle h, char *buf, uint32_t bufcap) {
    if (!_handle_valid(h) || _handle_pool(h) != p->pool_id) return 0;
    uint32_t pos = _handle_rank(h);
    uint8_t  gen = _handle_gen(h);
    LongEntry *eh = &p->headers[pos];
    uint32_t s0, s1, len;
    do {
        do { s0 = atomic_load_explicit(&p->seq, memory_order_acquire); }
        while (s0 & 1);   /* writer in progress: retry */
        uint64_t hdr = __atomic_load_n((uint64_t *)eh, __ATOMIC_ACQUIRE);
        if ((uint8_t)hdr != gen) return 0;   /* stale handle */
        len = (uint16_t)(hdr >> 32);
        if (len > bufcap) return 0;
        memcpy(buf, _long_entry_data(p, eh), len);
        s1 = atomic_load_explicit(&p->seq, memory_order_acquire);
    } while (s0 != s1);
    return len;
}

/* ----------------------------------------------------------------------
 * HYBRID POOL
 * ---------------------------------------------------------------------- */
typedef struct {
    ShortPool *short_pool;
    LongPool  *long_pool;
    uint16_t   base_pool_id;
} HybridPool;

static HybridPool *hybrid_create(uint16_t base_pool_id,
                                 uint32_t expected_short,
                                 uint32_t expected_long) {
    uint64_t seed = _entropy();
    HybridPool *hp = (HybridPool *)calloc(1, sizeof(HybridPool));
    assert(hp);
    hp->base_pool_id = base_pool_id;
    hp->short_pool = short_pool_create((uint16_t)(base_pool_id * 2 + 1), expected_short, seed);
    hp->long_pool  = long_pool_create ((uint16_t)(base_pool_id * 2),     expected_long,  seed);
    return hp;
}

static void hybrid_destroy(HybridPool *hp) {
    if (hp->short_pool) short_pool_destroy(hp->short_pool);
    if (hp->long_pool)  long_pool_destroy(hp->long_pool);
    free(hp);
}

static SF_Handle hybrid_intern(HybridPool *hp, const char *s, uint32_t len) {
    if (len <= SHORT_MAX_LEN) {
        SF_Handle h = short_intern(hp->short_pool, s, len);
        if (_handle_valid(h)) return h;
    }
    return long_intern(hp->long_pool, s, len);
}

static void hybrid_evict(HybridPool *hp, SF_Handle h) {
    if (!_handle_valid(h)) return;
    if (_handle_pool(h) & 1) short_evict(hp->short_pool, h);
    else                     long_evict(hp->long_pool, h);
}

static uint32_t hybrid_str(HybridPool *hp, SF_Handle h, char *buf, uint32_t bufcap) {
    if (!_handle_valid(h)) return 0;
    if (_handle_pool(h) & 1) return short_str(hp->short_pool, h, buf, bufcap);
    else                     return long_str(hp->long_pool, h, buf, bufcap);
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

/* ----------------------------------------------------------------------
 * CLEANUP MACRO
 * ---------------------------------------------------------------------- */
static inline void _hybrid_cleanup(HybridPool **hp) { if (*hp) { hybrid_destroy(*hp); *hp = NULL; } }
#define HYBRID_SCOPED(name, id, short_n, long_n) \
    HybridPool *name __attribute__((cleanup(_hybrid_cleanup))) = hybrid_create((id), (short_n), (long_n))
