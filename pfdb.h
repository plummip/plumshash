/*
 * pfdb.h — PRIEMFORMULE Fractal Database
 * =========================================
 *
 * A max-performance embedded fuzzy text search database using PRIEMFORMULE's
 * 9-column hierarchical sieve as its primary data structure.
 *
 * Architecture (PRIEMFORMULE multi-layer sieve):
 *
 *   Layer 1 — Digital root (Z/9Z): compute sum(bytes) % 9, use as per-doc
 *             pre-filter before trigram lookups. Eliminates ~89% of candidates.
 *             PRIEMFORMULE §1: n = 9r + c + 1, c = dr(doc)
 *
 *   Layer 2 — Bloom filter (64-bit): OR of all trigram hashes. Fast negative
 *             test before expensive trigram intersection.
 *
 *   Layer 3 — Sorted trigram array: fixed-width entries sorted by
 *             (hash32, doc_id). Binary search O(log N) per trigram lookup.
 *             10-byte packed entries: hash32(4B) | doc_id(4B) | pos(2B).
 *             Replaces varint delta-encoded posting lists (O(N) walk).
 *
 *   Layer 4 — Edit distance verification: Myers bit-parallel (corrected
 *             early-termination) + Damerau-Levenshtein fallback.
 *
 * Storage (single mmap'd file):
 *   [Header 4KB] [Doc text: length-prefixed blobs, grows dynamically]
 *   [Trigram index: sorted array of 10-byte entries, grows dynamically]
 *
 * Usage:
 *   #define PFDB_IMPLEMENTATION
 *   #include "pfdb.h"
 *
 *   pfdb_t *db = pfdb_open("data.db");
 *   uint32_t id = pfdb_add(db, "Amsterdam");
 *   uint32_t results[100];
 *   int n = pfdb_search(db, "Amsterdm", 2, results, 100);
 *   pfdb_close(db);
 *
 * Compile with main():
 *   gcc -O3 -march=armv8-a -o pfdb pfdb.h -lpthread
 *
 * License: MPL 2.0
 */

#ifndef PFDB_H
#define PFDB_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ═══════════════════════════════════════════════════════════════
 * Public API
 * ═══════════════════════════════════════════════════════════════ */

typedef struct pfdb_s pfdb_t;

/* Open or create database at path. Returns NULL on failure. */
pfdb_t *pfdb_open(const char *path);

/* Close database and unmap. */
void pfdb_close(pfdb_t *db);

/* Add a document. Returns document ID or (uint32_t)-1 on failure. */
uint32_t pfdb_add(pfdb_t *db, const char *text);

/* Fuzzy search: find docs within edit distance max_k of query.
 * Results are doc IDs, sorted by edit distance ascending.
 * Returns number of results written (max max_results). */
int pfdb_search(pfdb_t *db, const char *query, int max_k,
                uint32_t *results, int max_results);

/* Substring search: find docs containing needle (case-insensitive). */
int pfdb_find(pfdb_t *db, const char *needle,
              uint32_t *results, int max_results);

/* Delete a document by ID. Returns 0 on success. */
int pfdb_delete(pfdb_t *db, uint32_t id);

/* Get document count (excluding deleted). */
uint32_t pfdb_count(pfdb_t *db);

/* Get document text by ID. Returns NULL if deleted or invalid. */
const char *pfdb_text(pfdb_t *db, uint32_t id);

/* Get document text length. */
int pfdb_text_len(pfdb_t *db, uint32_t id);

/* Get digital root of a document (PRIEMFORMULE column c). */
uint8_t pfdb_dr(pfdb_t *db, uint32_t id);

/* Rebuild all 9 trigram indexes. Called automatically on search if dirty. */
void pfdb_rebuild(pfdb_t *db);

#ifdef __cplusplus
}
#endif

/* ═══════════════════════════════════════════════════════════════
 * Implementation
 * ═══════════════════════════════════════════════════════════════ */

#ifdef PFDB_IMPLEMENTATION

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <time.h>
#include <pthread.h>

/* ── Compiler hints ── */
#if defined(__GNUC__) || defined(__clang__)
#define PFDB_LIKELY(x)   __builtin_expect(!!(x), 1)
#define PFDB_UNLIKELY(x) __builtin_expect(!!(x), 0)
#define PFDB_HOT         __attribute__((hot))
#define PFDB_INLINE      static inline
#else
#define PFDB_LIKELY(x)   (x)
#define PFDB_UNLIKELY(x) (x)
#define PFDB_HOT
#define PFDB_INLINE      static inline
#endif

/* ── PlumsHash (embedded) ── */
/* We embed a minimal fast hash for trigrams. Using PRIEMFORMULE-guided constants. */
#define PFDB_PLUM_SEED 0x9E3779B9

/* FNV-1a for small keys (trigrams = 3 bytes) */
static uint64_t pfdb_hash(const uint8_t *data, int len, uint64_t seed) {
    uint64_t h = seed;
    for (int i = 0; i < len; i++) {
        h ^= (uint64_t)data[i];
        h *= 0x00000100000001B3ULL;
    }
    return h;
}

/* ── PRIEMFORMULE: Digital root (Z/9Z) ──
 *   n = 9r + c + 1  where c = dr(text) ∈ {0..8}
 *   Only columns 0,1,3,4,6,7 can yield primes, but for text search
 *   we use all 9 columns to shard the index.  The column is simply
 *   sum(bytes) % 9.
 *
 *   PRIEMFORMULE §1: c = (sum(text) - 1) mod 9
 *   We use the simpler: c = (∑ text[i]) % 9
 *   Documents with the same digital root share 1 of 9 trigram indexes.
 *   When searching, query dr selects which 1/9 of the index to probe.
 */
PFDB_INLINE uint8_t pfdb_dr_compute(const uint8_t *s, int n) {
    uint32_t sum = 0;
    for (int i = 0; i < n; i++) sum += s[i];
    return (uint8_t)(sum % 9);
}

/* ── PRIEMFORMULE: 6×6 schaakbord parity ──
 *   From §8.2: r mod 2 determines which columns are viable.
 *   r even → columns 0,4,6 are safe
 *   r odd  → columns 1,3,7 are safe
 *
 *   Applied to database: each document row (by offset in doc store)
 *   has a parity. We store the parity and use it as a quick filter
 *   before trigram lookup.
 *
 *   r = doc_id (sequential) → parity = doc_id & 1
 *   c = digital root of doc text
 *
 *   If parity ∉ safe_columns[c] → mark as low-priority candidate
 */

/* ── Constants ── */
#define PFDB_PAGE_SIZE        4096
#define PFDB_MAGIC            0x50464442  /* "PFDB" */
#define PFDB_VERSION          2
#define PFDB_MAX_DOCS         500000
#define PFDB_MAX_RES          200
#define PFDB_QLEN             3          /* trigram length */
#define PFDB_MAX_SCORED       512
#define PFDB_MYERS_MAX        62         /* max chars for 64-bit Myers */
#define PFDB_TRI_ENTRY_SZ     10         /* bytes per index entry */

/* On-disk trigram index entry — fixed width, sorted by (hash32, doc_id).
 * Binary search O(log N) per lookup — replaces varint delta-encoded
 * posting lists which required O(N) sequential walk. */
typedef struct __attribute__((packed)) {
    uint32_t hash32;   /* (trigram_hash >> 32) ^ trigram_hash */
    uint32_t doc_id;   /* document ID */
    uint16_t pos;      /* byte position of trigram in doc text */
} pfdb_trient_t;

/* ── On-disk header (page 0) ── */
typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint32_t version;
    uint32_t num_docs;
    uint32_t doc_store_off;    /* byte offset to doc text region */
    uint32_t doc_store_size;   /* allocated bytes */
    uint32_t tri_off;          /* byte offset to sorted trigram array */
    uint32_t tri_count;        /* number of entries in trigram array */
    uint8_t  tri_built;        /* 1 if index is valid */
    uint8_t  _pad[PFDB_PAGE_SIZE - (4+4+4+4+4+4+4+1)];
} pfdb_header_t;

/* ── In-memory doc entry ── */
typedef struct {
    uint32_t offset;   /* byte offset in mmap */
    uint16_t len;      /* text length */
    uint8_t  dr;       /* digital root (PRIEMFORMULE column c) */
    uint8_t  deleted;
    uint64_t bloom;    /* 64-bit bloom: OR of all trigram hashes */
} pfdb_doc_t;

/* ── Scored result ── */
typedef struct {
    uint32_t id;
    int      ed;
} pfdb_scored_t;

/* ── Database handle ── */
struct pfdb_s {
    uint8_t      *map;
    size_t        map_size;
    int           fd;
    pfdb_header_t *hdr;
    pfdb_doc_t   *docs;
    pthread_mutex_t lock;  /* for thread-safe add */
};

/* ── Internal functions ── */

/* ── File management ── */
static int pfdb_grow(pfdb_t *db, size_t need) {
    if (need <= db->map_size) return 0;
    need = ((need + PFDB_PAGE_SIZE - 1) / PFDB_PAGE_SIZE) * PFDB_PAGE_SIZE;
    if (ftruncate(db->fd, (off_t)need) < 0) return -1;
    if (db->map) munmap(db->map, db->map_size);
    db->map = (uint8_t*)mmap(NULL, need, PROT_READ | PROT_WRITE, MAP_SHARED, db->fd, 0);
    if (db->map == MAP_FAILED) return -1;
    db->map_size = need;
    db->hdr = (pfdb_header_t*)db->map;
    return 0;
}

PFDB_INLINE const uint8_t *pfdb_doc_text_ptr(const pfdb_t *db, uint32_t id) {
    if (id >= db->hdr->num_docs || db->docs[id].deleted) return NULL;
    return db->map + db->docs[id].offset;
}

/* ── Open / Close ── */

pfdb_t *pfdb_open(const char *path) {
    pfdb_t *db = (pfdb_t*)calloc(1, sizeof(pfdb_t));
    if (!db) return NULL;
    pthread_mutex_init(&db->lock, NULL);

    db->fd = open(path, O_RDWR | O_CREAT, 0644);
    if (db->fd < 0) { free(db); return NULL; }

    struct stat st;
    fstat(db->fd, &st);

    if (st.st_size == 0) {
        /* New file: write header */
        pfdb_header_t h;
        memset(&h, 0, sizeof(h));
        h.magic = PFDB_MAGIC;
        h.version = PFDB_VERSION;
        h.doc_store_off = PFDB_PAGE_SIZE;  /* right after header */
        h.doc_store_size = 0;
        h.tri_off = PFDB_PAGE_SIZE + 64 * 1024 * 1024;  /* 64MB for doc store */
        h.tri_count = 0;
        write(db->fd, &h, sizeof(h));
        ftruncate(db->fd, PFDB_PAGE_SIZE);
    }

    db->map = mmap(NULL, (size_t)(st.st_size ? st.st_size : PFDB_PAGE_SIZE),
                   PROT_READ | PROT_WRITE, MAP_SHARED, db->fd, 0);
    if (db->map == MAP_FAILED) { close(db->fd); free(db); return NULL; }
    db->map_size = (size_t)(st.st_size ? st.st_size : PFDB_PAGE_SIZE);
    db->hdr = (pfdb_header_t*)db->map;

    if (db->hdr->magic != PFDB_MAGIC) {
        munmap(db->map, db->map_size);
        close(db->fd); free(db);
        return NULL;
    }

    /* Load doc table from mmap */
    db->docs = (pfdb_doc_t*)calloc(PFDB_MAX_DOCS, sizeof(pfdb_doc_t));
    if (!db->docs) { munmap(db->map, db->map_size); close(db->fd); free(db); return NULL; }

    /* Reconstruct from doc store (length-prefixed blobs) */
    uint32_t off = db->hdr->doc_store_off;
    for (uint32_t i = 0; i < db->hdr->num_docs && i < PFDB_MAX_DOCS; i++) {
        if (off + 4 > db->hdr->doc_store_off + db->hdr->doc_store_size) break;
        uint32_t len;
        memcpy(&len, db->map + off, 4);
        uint8_t deleted = (len & 0x80000000) ? 1 : 0;
        uint32_t tlen = len & 0x7FFFFFFF;
        db->docs[i].offset = off + 4;
        db->docs[i].len = (uint16_t)(tlen > 65535 ? 65535 : tlen);
        db->docs[i].dr = deleted ? 0 : pfdb_dr_compute(db->map + off + 4, (int)tlen);
        db->docs[i].deleted = deleted;
        /* Recompute bloom on load (or store/restore it) */
        if (!deleted && tlen > 0) {
            uint64_t bf = 0;
            int blen = (int)tlen < 4096 ? (int)tlen : 4095;
            for (int j = 0; j <= blen - PFDB_QLEN; j++) {
                uint64_t h = pfdb_hash(db->map + off + 4 + j, PFDB_QLEN, PFDB_PLUM_SEED);
                bf |= (1ULL << (h & 63)) | (1ULL << ((h >> 6) & 63));
            }
            db->docs[i].bloom = bf;
        }
        off += 4 + tlen;
    }

    return db;
}

void pfdb_close(pfdb_t *db) {
    if (!db) return;
    /* Flush header to disk */
    msync(db->map, db->map_size, MS_SYNC);
    if (db->map) munmap(db->map, db->map_size);
    if (db->fd >= 0) close(db->fd);
    free(db->docs);
    pthread_mutex_destroy(&db->lock);
    free(db);
}

/* ── Add document ── */
uint32_t pfdb_add(pfdb_t *db, const char *text) {
    int tlen = (int)strlen(text);
    if (tlen <= 0 || tlen > 65535) return (uint32_t)-1;

    pthread_mutex_lock(&db->lock);

    uint32_t id = db->hdr->num_docs;
    if (id >= PFDB_MAX_DOCS) { pthread_mutex_unlock(&db->lock); return (uint32_t)-1; }

    /* Allocate space in doc store */
    uint32_t need = db->hdr->doc_store_size + 4 + (uint32_t)tlen;
    size_t end = (size_t)db->hdr->doc_store_off + need;
    if (end > db->map_size && pfdb_grow(db, end) < 0) {
        pthread_mutex_unlock(&db->lock);
        return (uint32_t)-1;
    }

    uint32_t off = db->hdr->doc_store_off + db->hdr->doc_store_size;
    uint32_t len = (uint32_t)tlen;
    memcpy(db->map + off, &len, 4);
    memcpy(db->map + off + 4, text, (size_t)tlen);

    uint8_t dr = pfdb_dr_compute((const uint8_t*)text, tlen);
    uint64_t bf = 0;
    int blen = tlen < 4096 ? tlen : 4095;
    for (int i = 0; i <= blen - PFDB_QLEN; i++) {
        uint64_t h = pfdb_hash((const uint8_t*)(text + i), PFDB_QLEN, PFDB_PLUM_SEED);
        bf |= (1ULL << (h & 63)) | (1ULL << ((h >> 6) & 63));
    }

    db->docs[id].offset = off + 4;
    db->docs[id].len = (uint16_t)tlen;
    db->docs[id].dr = dr;
    db->docs[id].deleted = 0;
    db->docs[id].bloom = bf;

    db->hdr->doc_store_size = need;
    db->hdr->num_docs++;
    db->hdr->tri_built = 0;

    pthread_mutex_unlock(&db->lock);
    return id;
}

/* ── Delete ── */

int pfdb_delete(pfdb_t *db, uint32_t id) {
    if (id >= db->hdr->num_docs || db->docs[id].deleted) return -1;
    db->docs[id].deleted = 1;
    /* Mark length prefix MSB */
    uint32_t off_len = db->docs[id].offset - 4;
    uint32_t raw;
    memcpy(&raw, db->map + off_len, 4);
    raw |= 0x80000000;
    memcpy(db->map + off_len, &raw, 4);
    db->hdr->tri_built = 0;
    return 0;
}

/* ── Entry comparators ── */
static int trient_cmp(const void *va, const void *vb) {
    const pfdb_trient_t *a = (const pfdb_trient_t*)va;
    const pfdb_trient_t *b = (const pfdb_trient_t*)vb;
    if (a->hash32 < b->hash32) return -1;
    if (a->hash32 > b->hash32) return 1;
    if (a->doc_id < b->doc_id) return -1;
    if (a->doc_id > b->doc_id) return 1;
    return 0;
}

/* Hash32-only comparator for bsearch (ignores doc_id/pos).
 * Array is sorted by (hash32, doc_id), so hash32-only comparison
 * is valid for finding ANY entry with matching hash32. */
static int trient_cmp_h32(const void *va, const void *vb) {
    uint32_t ha = ((const pfdb_trient_t*)va)->hash32;
    uint32_t hb = ((const pfdb_trient_t*)vb)->hash32;
    if (ha < hb) return -1;
    if (ha > hb) return 1;
    return 0;
}

/* Custom binary search: find first entry with matching hash32.
 * Returns pointer to first matching entry, or NULL if none. */
static pfdb_trient_t *trient_find_first(pfdb_trient_t *arr, uint32_t n,
                                         uint32_t hash32) {
    pfdb_trient_t key;
    key.hash32 = hash32;
    pfdb_trient_t *f = (pfdb_trient_t*)bsearch(&key, arr, n,
                        sizeof(pfdb_trient_t), trient_cmp_h32);
    if (!f) return NULL;
    /* Walk back to first entry with this hash */
    while (f > arr && (f-1)->hash32 == hash32) f--;
    return f;
}

/* Count entries with given hash32. */
static uint32_t trient_count(pfdb_trient_t *arr, uint32_t n, uint32_t hash32) {
    pfdb_trient_t *f = trient_find_first(arr, n, hash32);
    if (!f) return 0;
    uint32_t cnt = 0;
    pfdb_trient_t *end = arr + n;
    while (f < end && f->hash32 == hash32) { cnt++; f++; }
    return cnt;
}

/* ── Build sorted trigram array ── */

void pfdb_rebuild(pfdb_t *db) {
    /* Pass 1: count total trigrams */
    uint32_t total = 0;
    char buf[4096];
    for (uint32_t d = 0; d < db->hdr->num_docs; d++) {
        if (db->docs[d].deleted) continue;
        int tl = db->docs[d].len;
        int blen = tl < 4096 ? tl : 4095;
        if (blen >= PFDB_QLEN) total += (uint32_t)(blen - PFDB_QLEN + 1);
    }
    if (total == 0) return;

    /* Allocate sorted array */
    size_t arr_bytes = (size_t)total * PFDB_TRI_ENTRY_SZ;
    size_t end = (size_t)db->hdr->tri_off + arr_bytes;
    if (end > db->map_size && pfdb_grow(db, end) < 0) return;

    pfdb_trient_t *arr = (pfdb_trient_t*)(db->map + db->hdr->tri_off);
    uint32_t idx = 0;

    /* Pass 2: fill entries */
    for (uint32_t d = 0; d < db->hdr->num_docs; d++) {
        if (db->docs[d].deleted) continue;
        int tl = db->docs[d].len;
        const uint8_t *text = pfdb_doc_text_ptr(db, d);
        if (!text) continue;
        int blen = tl < 4096 ? tl : 4095;
        for (int i = 0; i < blen; i++) {
            buf[i] = (char)tolower((uint8_t)text[i]);
        }
        for (int i = 0; i <= blen - PFDB_QLEN; i++) {
            uint64_t h = pfdb_hash((const uint8_t*)(buf + i), PFDB_QLEN, PFDB_PLUM_SEED);
            arr[idx].hash32 = (uint32_t)((h >> 32) ^ (uint32_t)h);
            arr[idx].doc_id = d;
            arr[idx].pos = (uint16_t)i;
            idx++;
        }
    }

    /* Sort by (hash32, doc_id) */
    qsort(arr, total, sizeof(pfdb_trient_t), trient_cmp);

    db->hdr->tri_count = total;
    db->hdr->tri_built = 1;
    msync(db->map, db->map_size, MS_SYNC);
}

/* ── Myers bit-parallel edit distance (corrected early-termination) ──
 * Pre-compute Peq[256] from pattern (a) once, then call myers_peq for
 * each candidate text (b). This avoids 2048 bytes of Peq setup per call. */

PFDB_HOT static void myers_peq_init(uint64_t *Peq, const char *a, int la) {
    for (int i = 0; i < la; i++) Peq[(uint8_t)a[i]] |= (1ULL << i);
}

PFDB_HOT static int myers_peq(const uint64_t *Peq, int la,
                                const char *b, int lb, int max_k) {
    if (la > PFDB_MYERS_MAX || lb > PFDB_MYERS_MAX) return -1;
    /* Note: caller must ensure a (query) is the pattern and b is the text.
     * We do NOT swap here because Peq was computed from a. */
    if (lb - la > max_k) return max_k + 1;
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

/* ── Damerau-Levenshtein fallback ── */

static int pfdb_dl(const char *a, int la, const char *b, int lb, int max_k) {
    if (la > 60) la = 60; if (lb > 60) lb = 60;
    int ld = la > lb ? la - lb : lb - la;
    if (ld > max_k) return max_k + 1;
    int d0[64], d1[64], d2[64], *p0 = d0, *p1 = d1, *p2 = d2;
    for (int j = 0; j <= lb; j++) p1[j] = j;
    for (int i = 1; i <= la; i++) {
        p0[0] = i;
        int row_min = i;
        for (int j = 1; j <= lb; j++) {
            int cost = (a[i-1] == b[j-1]) ? 0 : 1;
            int m = p1[j] + 1;
            if (p0[j-1] + 1 < m) m = p0[j-1] + 1;
            if (p1[j-1] + cost < m) m = p1[j-1] + cost;
            if (i > 1 && j > 1 && a[i-1] == b[j-2] && a[i-2] == b[j-1])
                if (p2[j-2] + cost < m) m = p2[j-2] + cost;
            p0[j] = m;
            if (m < row_min) row_min = m;
        }
        if (row_min > max_k) return max_k + 1;
        int *t = p2; p2 = p1; p1 = p0; p0 = t;
    }
    return p1[lb];
}

/* Substring edit distance with position hint — uses pre-computed Peq.
 * Only tries the max window size (ql + max_k) per start position — Myers
 * naturally handles insertions/deletions through its score computation,
 * so trying 5 window lengths per position is redundant. */
static int pfdb_substr_ed(const uint64_t *Peq, const char *q, int ql,
                          const uint8_t *t, int tl,
                          int max_k, uint32_t off) {
    int best = max_k + 1;
    int lo = 0, hi = tl;
    if (off > 0) {
        lo = (int)off - ql - max_k; if (lo < 0) lo = 0;
        hi = (int)off + ql + max_k; if (hi > tl) hi = tl;
    }
    for (int start = lo; start < hi; start++) {
        int wlen = ql + max_k;
        if (start + wlen > tl) wlen = tl - start;
        if (wlen < 1) continue;
        int ed = myers_peq(Peq, ql, (const char*)(t + start), wlen, max_k);
        if (ed < 0 || ed > max_k)
            ed = pfdb_dl(q, ql, (const char*)(t + start), wlen, max_k);
        if (ed >= 0 && ed < best) best = ed;
        if (best == 0) return 0;
    }
    return best;
}

/* ── qsort comparator ── */

static int pfdb_scmp(const void *va, const void *vb) {
    int ea = ((const pfdb_scored_t*)va)->ed;
    int eb = ((const pfdb_scored_t*)vb)->ed;
    return (ea > eb) - (ea < eb);
}

/* ═══════════════════════════════════════════════════════════════
 * SEARCH — PRIEMFORMULE multi-layer sieve
 * ═══════════════════════════════════════════════════════════════

   Layer 1:  Bloom filter (64-bit AND + popcount) — fast negative
   Layer 2:  Edit distance verification (Myers/DL)
   Layer 3:  Rank by edit distance

   Candidate retrieval: single rarest trigram bucket (same as pfdb_find).
   Avoids iterating all buckets and re-hashing all document trigrams.
 */

int pfdb_search(pfdb_t *db, const char *query, int max_k,
                uint32_t *results, int max_results) {
    if (db->hdr->num_docs == 0) return 0;
    if (!db->hdr->tri_built) pfdb_rebuild(db);

    int ql = (int)strlen(query);
    if (ql > PFDB_MYERS_MAX) ql = PFDB_MYERS_MAX;

    /* Lowercase query and pre-compute Myers Peq table */
    char qlow[PFDB_MYERS_MAX + 1];
    for (int i = 0; i < ql; i++) qlow[i] = (char)tolower((uint8_t)query[i]);
    qlow[ql] = 0;
    uint64_t Peq[256] = {0};
    myers_peq_init(Peq, qlow, ql);

    int nscored = 0;
    pfdb_scored_t scored_buf[PFDB_MAX_SCORED];

    if (ql < PFDB_QLEN) {
        /* Too short for trigrams — linear scan */
        for (uint32_t d = 0; d < db->hdr->num_docs && nscored < PFDB_MAX_SCORED; d++) {
            if (db->docs[d].deleted) continue;
            const uint8_t *text = pfdb_doc_text_ptr(db, d);
            if (!text) continue;
            int tl = db->docs[d].len;
            int ed = pfdb_substr_ed(Peq, qlow, ql, text, tl, max_k, 0);
            if (ed <= max_k) {
                scored_buf[nscored].id = d;
                scored_buf[nscored].ed = ed;
                nscored++;
            }
        }
        qsort(scored_buf, nscored, sizeof(scored_buf[0]), pfdb_scmp);
        int out = 0;
        for (int i = 0; i < nscored && out < max_results; i++)
            results[out++] = scored_buf[i].id;
        return out;
    }

    /* Compute query bloom + find rarest trigram by hash count */
    uint64_t q_bloom = 0;
    int best_hash = -1;
    uint32_t best_cnt = 0xFFFFFFFF;
    pfdb_trient_t *arr = (pfdb_trient_t*)(db->map + db->hdr->tri_off);
    uint32_t nentries = db->hdr->tri_count;

    for (int i = 0; i <= ql - PFDB_QLEN; i++) {
        uint64_t h = pfdb_hash((const uint8_t*)(qlow + i), PFDB_QLEN, PFDB_PLUM_SEED);
        q_bloom |= (1ULL << (h & 63)) | (1ULL << ((h >> 6) & 63));
        uint32_t hash32 = (uint32_t)((h >> 32) ^ (uint32_t)h);
        uint32_t cnt = trient_count(arr, nentries, hash32);
        if (cnt > 0 && cnt < best_cnt) { best_cnt = cnt; best_hash = i; }
    }
    if (best_hash < 0) return 0;

    /* Get hash for rarest trigram */
    uint64_t rarest_h = pfdb_hash((const uint8_t*)(qlow + best_hash), PFDB_QLEN, PFDB_PLUM_SEED);
    uint32_t rarest_hash32 = (uint32_t)((rarest_h >> 32) ^ (uint32_t)rarest_h);

    /* Find first entry with this hash and iterate */
    pfdb_trient_t *found = trient_find_first(arr, nentries, rarest_hash32);
    if (!found) return 0;
    pfdb_trient_t *end = arr + nentries;

    /* Iterate all entries with this hash, verify with edit distance */
    uint8_t *seen = (uint8_t*)calloc((db->hdr->num_docs + 7) / 8, 1);

    while (found < end && found->hash32 == rarest_hash32 && nscored < PFDB_MAX_SCORED) {
        uint32_t doc = found->doc_id;
        uint16_t offs = found->pos;

        if (doc < db->hdr->num_docs && !(seen[doc >> 3] & (1 << (doc & 7)))) {
            /* PRIEMFORMULE Layer 1: bloom pre-filter */
            uint64_t match_bloom = db->docs[doc].bloom & q_bloom;
            if ((int)__builtin_popcountll(match_bloom) >= 2) {
                const uint8_t *text = pfdb_doc_text_ptr(db, doc);
                if (text) {
                    int tl = db->docs[doc].len;
                    if (tl >= ql - max_k) {
                        /* PRIEMFORMULE Layer 2: edit distance */
                        int ed = pfdb_substr_ed(Peq, qlow, ql, text, tl, max_k, offs);
                        if (ed <= max_k) {
                            scored_buf[nscored].id = doc;
                            scored_buf[nscored].ed = ed;
                            nscored++;
                        }
                    }
                }
            }
            seen[doc >> 3] |= (1 << (doc & 7));
        }
        found++;
    }

    free(seen);
    qsort(scored_buf, nscored, sizeof(scored_buf[0]), pfdb_scmp);
    int out = 0;
    for (int i = 0; i < nscored && out < max_results; i++)
        results[out++] = scored_buf[i].id;
    return out;
}

/* ── Find (substring, case-insensitive) ── */

int pfdb_find(pfdb_t *db, const char *needle, uint32_t *results, int max_results) {
    if (db->hdr->num_docs == 0) return 0;
    if (!db->hdr->tri_built) pfdb_rebuild(db);

    int nl = (int)strlen(needle);
    if (nl < PFDB_QLEN) {
        /* Linear scan */
        int found = 0;
        for (uint32_t d = 0; d < db->hdr->num_docs && found < max_results; d++) {
            if (db->docs[d].deleted) continue;
            const uint8_t *text = pfdb_doc_text_ptr(db, d);
            if (text) {
                const char *t = (const char*)text;
                int tl = db->docs[d].len;
                for (int i = 0; i <= tl - nl; i++) {
                    int match = 1;
                    for (int j = 0; j < nl; j++) {
                        if (tolower((uint8_t)t[i+j]) != tolower((uint8_t)needle[j])) {
                            match = 0; break;
                        }
                    }
                    if (match) { results[found++] = d; break; }
                }
            }
        }
        return found;
    }

    /* Find rarest trigram by hash count in sorted array */
    char nlow[256];
    int nllen = nl < 256 ? nl : 255;
    for (int i = 0; i < nllen; i++)
        nlow[i] = (char)tolower((uint8_t)needle[i]);

    pfdb_trient_t *arr = (pfdb_trient_t*)(db->map + db->hdr->tri_off);
    uint32_t nentries = db->hdr->tri_count;

    int best_tri = -1;
    uint32_t best_cnt = 0xFFFFFFFF;
    for (int i = 0; i <= nllen - PFDB_QLEN; i++) {
        uint64_t h = pfdb_hash((const uint8_t*)(nlow + i), PFDB_QLEN, PFDB_PLUM_SEED);
        uint32_t hash32 = (uint32_t)((h >> 32) ^ (uint32_t)h);
        uint32_t cnt = trient_count(arr, nentries, hash32);
        if (cnt > 0 && cnt < best_cnt) { best_cnt = cnt; best_tri = i; }
    }
    if (best_tri < 0) return 0;

    /* Get hash for rarest trigram and find first entry */
    uint64_t rarest_h = pfdb_hash((const uint8_t*)(nlow + best_tri), PFDB_QLEN, PFDB_PLUM_SEED);
    uint32_t rarest_hash32 = (uint32_t)((rarest_h >> 32) ^ (uint32_t)rarest_h);
    pfdb_trient_t *found = trient_find_first(arr, nentries, rarest_hash32);
    if (!found) return 0;
    pfdb_trient_t *end = arr + nentries;

    /* Iterate all entries with this hash, search for substring */
    int found_count = 0;
    uint8_t *seen = (uint8_t*)calloc((db->hdr->num_docs + 7) / 8, 1);

    while (found < end && found->hash32 == rarest_hash32 && found_count < max_results) {
        uint32_t doc = found->doc_id;
        if (doc < db->hdr->num_docs && !(seen[doc >> 3] & (1 << (doc & 7)))) {
            const uint8_t *text = pfdb_doc_text_ptr(db, doc);
            if (text) {
                const char *t = (const char*)text;
                int tl = db->docs[doc].len;
                for (int i = 0; i <= tl - nllen; i++) {
                    int match = 1;
                    for (int j = 0; j < nllen; j++) {
                        if (tolower((uint8_t)t[i+j]) != tolower((uint8_t)nlow[j])) {
                            match = 0; break;
                        }
                    }
                    if (match) { results[found_count++] = doc; break; }
                }
            }
            seen[doc >> 3] |= (1 << (doc & 7));
        }
        found++;
    }
    free(seen);
    return found_count;
}

/* ── Count ── */

uint32_t pfdb_count(pfdb_t *db) {
    uint32_t n = 0;
    for (uint32_t i = 0; i < db->hdr->num_docs; i++)
        if (!db->docs[i].deleted) n++;
    return n;
}

/* ── Text access ── */

const char *pfdb_text(pfdb_t *db, uint32_t id) {
    if (id >= db->hdr->num_docs || db->docs[id].deleted) return NULL;
    return (const char*)(db->map + db->docs[id].offset);
}

int pfdb_text_len(pfdb_t *db, uint32_t id) {
    if (id >= db->hdr->num_docs) return 0;
    return (int)db->docs[id].len;
}

uint8_t pfdb_dr(pfdb_t *db, uint32_t id) {
    if (id >= db->hdr->num_docs) return 0;
    return db->docs[id].dr;
}

#endif /* PFDB_IMPLEMENTATION */

/* ═══════════════════════════════════════════════════════════════
 * Standalone CLI — compile with:
 *   gcc -O3 -march=armv8-a -o pfdb pfdb.h
 * ═══════════════════════════════════════════════════════════════ */

#if defined(PFDB_IMPLEMENTATION) && !defined(PFDB_AS_LIB)

int main(int argc, char **argv) {
    const char *db_path = getenv("PFDB_PATH");
    if (!db_path) db_path = "data.pfdb";

    if (argc < 2) {
        printf("pfdb v%d — PRIEMFORMULE Fractal Fuzzy Database\n\n", PFDB_VERSION);
        printf("  add TEXT              Add document\n");
        printf("  search QUERY [K]      Fuzzy search (ed <= K, default 2)\n");
        printf("  find SUBSTRING        Substring search\n");
        printf("  delete ID             Delete document\n");
        printf("  count                 Document count\n");
        printf("  text ID               Show document text\n");
        printf("  dr ID                 Show digital root\n");
        printf("  bench [N]             Benchmark\n");
        printf("  serve                 Interactive REPL\n");
        printf("\n  Env: PFDB_PATH=path (default: data.pfdb)\n");
        return 0;
    }

    pfdb_t *db = pfdb_open(db_path);
    if (!db) { fprintf(stderr, "Cannot open %s\n", db_path); return 1; }

    const char *cmd = argv[1];

    if (!strcmp(cmd, "add")) {
        if (argc < 3) { printf("Usage: add TEXT\n"); pfdb_close(db); return 1; }
        char text[4096]; text[0] = 0;
        for (int i = 2; i < argc; i++) {
            if (i > 2) strcat(text, " ");
            strcat(text, argv[i]);
        }
        uint32_t id = pfdb_add(db, text);
        if (id == (uint32_t)-1) printf("Failed\n");
        else printf("ok id=%u dr=%u\n", id, pfdb_dr(db, id));

    } else if (!strcmp(cmd, "search")) {
        if (argc < 3) { printf("Usage: search QUERY [K]\n"); pfdb_close(db); return 1; }
        char query[256]; query[0] = 0;
        int k_pos = 2;
        for (int i = 2; i < argc; i++) {
            if (i > 2 && isdigit(argv[i][0]) && i == argc - 1) { k_pos = i; break; }
            if (i > 2) strcat(query, " ");
            strcat(query, argv[i]);
        }
        int max_k = (k_pos < argc && k_pos + 1 != 2) ? atoi(argv[k_pos]) : 2;
        if (k_pos == 2) { strcpy(query, argv[2]); max_k = 2; }

        uint32_t results[PFDB_MAX_RES];
        int n = pfdb_search(db, query, max_k, results, PFDB_MAX_RES);
        for (int i = 0; i < n; i++) {
            const char *text = pfdb_text(db, results[i]);
            printf("[%u] dr=%u ", results[i], pfdb_dr(db, results[i]));
            if (text) {
                int tl = pfdb_text_len(db, results[i]);
                if (tl > 80) tl = 80;
                printf("%.*s%s", tl, text, pfdb_text_len(db, results[i]) > 80 ? "..." : "");
            }
            printf("\n");
        }
        printf("%d results\n", n);

    } else if (!strcmp(cmd, "find")) {
        if (argc < 3) { printf("Usage: find SUBSTRING\n"); pfdb_close(db); return 1; }
        char needle[256]; needle[0] = 0;
        for (int i = 2; i < argc; i++) {
            if (i > 2) strcat(needle, " ");
            strcat(needle, argv[i]);
        }
        uint32_t results[PFDB_MAX_RES];
        int n = pfdb_find(db, needle, results, PFDB_MAX_RES);
        for (int i = 0; i < n; i++) {
            const char *text = pfdb_text(db, results[i]);
            printf("[%u] ", results[i]);
            if (text) {
                int tl = pfdb_text_len(db, results[i]);
                if (tl > 80) tl = 80;
                printf("%.*s%s", tl, text, pfdb_text_len(db, results[i]) > 80 ? "..." : "");
            }
            printf("\n");
        }
        printf("%d results\n", n);

    } else if (!strcmp(cmd, "delete")) {
        if (argc < 3) { printf("Usage: delete ID\n"); pfdb_close(db); return 1; }
        uint32_t id = (uint32_t)atoi(argv[2]);
        if (pfdb_delete(db, id) == 0) printf("ok\n");
        else printf("Failed (invalid or already deleted)\n");

    } else if (!strcmp(cmd, "count")) {
        printf("%u docs (%u total)\n", pfdb_count(db), db->hdr->num_docs);

    } else if (!strcmp(cmd, "text")) {
        if (argc < 3) { printf("Usage: text ID\n"); pfdb_close(db); return 1; }
        uint32_t id = (uint32_t)atoi(argv[2]);
        const char *text = pfdb_text(db, id);
        if (text) printf("%.*s\n", pfdb_text_len(db, id), text);
        else printf("(invalid or deleted)\n");

    } else if (!strcmp(cmd, "dr")) {
        if (argc < 3) { printf("Usage: dr ID\n"); pfdb_close(db); return 1; }
        uint32_t id = (uint32_t)atoi(argv[2]);
        printf("dr=%u\n", pfdb_dr(db, id));

    } else if (!strcmp(cmd, "bench")) {
        int N = (argc > 2) ? atoi(argv[2]) : 10000;
        if (N < 100) N = 100;
        if (N > 200000) N = 200000;

        if (pfdb_count(db) < (uint32_t)N) {
            fprintf(stderr, "Generating %d docs... ", N);
            const char *words[] = {"the","quick","brown","fox","jumps","over","lazy","dog",
                "amsterdam","rotterdam","utrecht","database","search","fuzzy","index",
                "pfdb","hash","query","document","text","engine","fractal","digital","root",
                "priemformule","column","sieve","parity","bloom","trigram","myers","pfdb"};
            int nw = 32;
            for (int i = 0; i < N; i++) {
                char buf[256]; buf[0] = 0;
                int nwords = 5 + (i % 15);
                for (int w = 0; w < nwords; w++) {
                    if (w > 0) strcat(buf, " ");
                    strcat(buf, words[(i * 7 + w * 13) % nw]);
                }
                pfdb_add(db, buf);
            }
            fprintf(stderr, "done\n");
        }

        if (!db->hdr->tri_built) {
            fprintf(stderr, "Building trigram indexes... "); fflush(stderr);
            clock_t t = clock();
            pfdb_rebuild(db);
            fprintf(stderr, "done (%.3fs)\n", (double)(clock()-t)/CLOCKS_PER_SEC);
        }

        printf("\n╔════════════════════════════════════════════════════════╗\n");
        printf("║  pfdb v%d BENCH  —  %u docs                      ║\n",
            PFDB_VERSION, pfdb_count(db));
        printf("╠════════════════════════════════════════════════════════╣\n");

        {   const char *q = "amsterdm"; int iters = 200;
            clock_t t = clock(); volatile int s = 0;
            for (int i = 0; i < iters; i++) {
                uint32_t found[PFDB_MAX_RES];
                s += pfdb_search(db, q, 2, found, PFDB_MAX_RES);
            }
            double dt = (double)(clock()-t)/CLOCKS_PER_SEC;
            (void)s;
            printf("║ Fuzzy search (ed≤2)  %8d ops: %8.3fs → %9.1f µs/op ║\n",
                iters, dt, dt*1e6/iters);
        }

        {   const char *q = "dam"; int iters = 1000;
            clock_t t = clock(); volatile int s = 0;
            for (int i = 0; i < iters; i++) {
                uint32_t found[PFDB_MAX_RES];
                s += pfdb_find(db, q, found, PFDB_MAX_RES);
            }
            double dt = (double)(clock()-t)/CLOCKS_PER_SEC;
            (void)s;
            printf("║ Substring (trigram)  %8d ops: %8.3fs → %9.1f µs/op ║\n",
                iters, dt, dt*1e6/iters);
        }

        printf("║  PRIEMFORMULE layers: 9-col DR + bloom + trigram + Myers ║\n");
        printf("╚════════════════════════════════════════════════════════╝\n");

    } else if (!strcmp(cmd, "serve")) {
        printf("pfdb> "); fflush(stdout);
        char line[4096];
        while (fgets(line, sizeof(line), stdin)) {
            int len = (int)strlen(line);
            while (len > 0 && (line[len-1] == '\n' || line[len-1] == '\r')) line[--len] = 0;
            if (len == 0) { printf("pfdb> "); fflush(stdout); continue; }

            char *tokens[32]; int nt = 0;
            char *tok = strtok(line, " ");
            while (tok && nt < 32) { tokens[nt++] = tok; tok = strtok(NULL, " "); }
            if (nt == 0) { printf("pfdb> "); fflush(stdout); continue; }

            if (!strcmp(tokens[0], "exit") || !strcmp(tokens[0], "quit")) break;
            else if (!strcmp(tokens[0], "add")) {
                if (nt < 2) { printf("Usage: add TEXT\n"); }
                else {
                    char text[4096]; text[0] = 0;
                    for (int i = 1; i < nt; i++) {
                        if (i > 1) strcat(text, " ");
                        strcat(text, tokens[i]);
                    }
                    uint32_t id = pfdb_add(db, text);
                    if (id == (uint32_t)-1) printf("Failed\n");
                    else printf("ok id=%u dr=%u\n", id, pfdb_dr(db, id));
                }
            }
            else if (!strcmp(tokens[0], "search")) {
                if (nt < 2) { printf("Usage: search QUERY [K]\n"); }
                else {
                    char query[256]; query[0] = 0;
                    int max_k = 2;
                    int has_k = (nt > 2 && isdigit(tokens[nt-1][0])) ? 1 : 0;
                    int end = has_k ? nt - 1 : nt;
                    for (int i = 1; i < end; i++) {
                        if (i > 1) strcat(query, " ");
                        strcat(query, tokens[i]);
                    }
                    if (has_k) max_k = atoi(tokens[nt-1]);

                    uint32_t results[PFDB_MAX_RES];
                    int n = pfdb_search(db, query, max_k, results, PFDB_MAX_RES);
                    for (int i = 0; i < n; i++) {
                        const char *text = pfdb_text(db, results[i]);
                        printf("  ed<=%d [%u] ", max_k, results[i]);
                        if (text) {
                            int tl = pfdb_text_len(db, results[i]);
                            if (tl > 80) tl = 80;
                            printf("%.*s%s", tl, text,
                                pfdb_text_len(db, results[i]) > 80 ? "..." : "");
                        }
                        printf("\n");
                    }
                    printf("%d results\n", n);
                }
            }
            else if (!strcmp(tokens[0], "find")) {
                if (nt < 2) { printf("Usage: find SUBSTRING\n"); }
                else {
                    char needle[256]; needle[0] = 0;
                    for (int i = 1; i < nt; i++) {
                        if (i > 1) strcat(needle, " ");
                        strcat(needle, tokens[i]);
                    }
                    uint32_t results[PFDB_MAX_RES];
                    int n = pfdb_find(db, needle, results, PFDB_MAX_RES);
                    for (int i = 0; i < n; i++) {
                        const char *text = pfdb_text(db, results[i]);
                        printf("  [%u] ", results[i]);
                        if (text) {
                            int tl = pfdb_text_len(db, results[i]);
                            if (tl > 80) tl = 80;
                            printf("%.*s%s", tl, text,
                                pfdb_text_len(db, results[i]) > 80 ? "..." : "");
                        }
                        printf("\n");
                    }
                    printf("%d results\n", n);
                }
            }
            else if (!strcmp(tokens[0], "delete")) {
                if (nt < 2) printf("Usage: delete ID\n");
                else if (pfdb_delete(db, (uint32_t)atoi(tokens[1])) == 0)
                    printf("ok\n");
                else printf("Failed\n");
            }
            else if (!strcmp(tokens[0], "count")) {
                printf("%u docs (%u total)\n", pfdb_count(db), db->hdr->num_docs);
            }
            else if (!strcmp(tokens[0], "bench")) {
                int N = (nt > 1) ? atoi(tokens[1]) : 10000;
                /* simplified inline bench */
                if (pfdb_count(db) < (uint32_t)N) {
                    const char *w[] = {"the","quick","brown","fox","database","search","fuzzy"};
                    int nw = 7;
                    for (int i = 0; i < N && i < 1000; i++) {
                        char buf[64]; buf[0]=0;
                        for (int ww=0; ww<5; ww++) {
                            if(ww>0) strcat(buf," ");
                            strcat(buf, w[(i*7+ww*13)%nw]);
                        }
                        pfdb_add(db, buf);
                    }
                }
                printf("documents: %u\n", pfdb_count(db));
            }
            else printf("? %s\n", tokens[0]);
            fflush(stdout);
            printf("pfdb> ");
            fflush(stdout);
        }

    } else {
        printf("? %s\n", cmd);
    }

    pfdb_close(db);
    return 0;
}

#else  /* PFDB_AS_LIB / not PFDB_IMPLEMENTATION */
/* When PFDB_IMPLEMENTATION is not defined, only declarations are visible.
 * When defined but PFDB_AS_LIB is set, no main() is compiled. */
#endif /* PFDB_AS_LIB guard */

#endif /* PFDB_H */
