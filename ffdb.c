/*
 * ffdb.c — Fractal Fuzzy DB: compact mmap'd fuzzy text search database.
 *
 * Architecture (inspired by fractal_fuzzy_db.c):
 *   Layer 1 — Digital root (Z/9Z): pre-filters ~89% of non-matches instantly.
 *             Computed as sum(bytes) % 9, with 0→9. Only 1/9 of entries share
 *             a digital root. This is the PRIEMFORMULE 9-column sieve applied
 *             to text: dr("abc") = ('a'+'b'+'c') % 9.
 *   Layer 2 — Trigram inverted index: compact posting lists as delta-encoded
 *             varints. Each posting = (doc_id, offset_in_doc).
 *   Layer 3 — Myers bit-parallel edit distance (≤64 chars) + Damerau fallback.
 *
 * Storage (single mmap'd file):
 *   [Header 4KB] [Doc offsets: N×8 bytes] [Doc text: newline-separated]
 *   [Trigram index: 1009×directory + varint posting lists]
 *
 * Commands:
 *   add TEXT              Add document
 *   search QUERY [K]      Fuzzy search (ed ≤ K, default 2)
 *   find SUBSTRING        Substring search
 *   count                 Document count
 *   bench [N]             Benchmark
 *   serve                 Interactive REPL
 *
 * Compile: gcc -O3 -march=armv8-a -o ffdb ffdb.c -I.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <ctype.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <time.h>

#define PLUMSHASH_IMPLEMENTATION
#include "plumshash.h"

/* ── Compiler ── */
#if defined(__GNUC__) || defined(__clang__)
#define LIKELY(x)   __builtin_expect(!!(x), 1)
#define ACHT_HOT    __attribute__((hot))
#define ACHT_INLINE __attribute__((always_inline)) static inline
#else
#define LIKELY(x)   (x)
#define ACHT_HOT
#define ACHT_INLINE static inline
#endif

/* ── Constants ── */
#define PAGE_SIZE        4096
#define MAGIC            0x46464442  /* "FFDB" */
#define VERSION          1
#define DB_FILE          "data.ffdb"
#define MAX_DOCS         500000
#define MAX_RES          200
#define N_BUCKETS        1009       /* prime */
#define QLEN             3
#define MAX_SCORED       512
#define MYERS_MAX        62  /* max safe 64-bit Myers (63 bits + overflow guard) */

/* ── Digital root (Z/9Z) — from PRIEMFORMULE ── */
ACHT_INLINE uint8_t digital_root(const uint8_t *s, int n) {
    uint32_t sum = 0;
    for (int i = 0; i < n; i++) sum += s[i];
    uint8_t r = sum % 9;
    return r ? r : 9;  /* 0→9 */
}

/* ── On-disk header ── */
typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint32_t version;
    uint32_t num_docs;
    uint32_t doc_text_off;    /* byte offset to doc text region */
    uint32_t doc_text_size;   /* allocated bytes */
    uint32_t tri_off;         /* byte offset to trigram index */
    uint32_t tri_size;        /* allocated bytes for trigram index */
    uint32_t tri_built;       /* 1 if index is valid */
    uint8_t  _pad[PAGE_SIZE - 32];
} Header;

/* ── In-memory doc entry ── */
typedef struct {
    uint32_t offset;   /* byte offset of text in mmap */
    uint16_t len;      /* text length */
    uint8_t  dr;       /* digital root (cached) */
    uint8_t  deleted;
    uint64_t bloom;    /* 64-bit bloom: OR of all trigram hashes */
} Doc;

/* ── Scored result ── */
struct scored { uint32_t id; int ed; };

/* ── Global state ── */
static uint8_t *map = NULL;
static size_t   map_size = 0;
static int      fd = -1;
static Header  *hdr = NULL;
static Doc     *docs = NULL;

/* ── File ops ── */
static int grow(size_t need) {
    if (need <= map_size) return 0;
    need = ((need + PAGE_SIZE - 1) / PAGE_SIZE) * PAGE_SIZE;
    if (ftruncate(fd, (off_t)need) < 0) return -1;
    if (map) munmap(map, map_size);
    map = mmap(NULL, need, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (map == MAP_FAILED) return -1;
    map_size = need;
    hdr = (Header*)map;
    return 0;
}

static int db_open(const char *path) {
    fd = open(path, O_RDWR | O_CREAT, 0644);
    if (fd < 0) return -1;
    struct stat st; fstat(fd, &st);
    if (st.st_size == 0) {
        Header h; memset(&h, 0, sizeof(h));
        h.magic = MAGIC; h.version = VERSION;
        h.doc_text_off = PAGE_SIZE + MAX_DOCS * 8;  /* after doc offset table */
        h.tri_off = h.doc_text_off + 16 * 1024 * 1024;  /* 16MB for text */
        write(fd, &h, sizeof(h));
        ftruncate(fd, PAGE_SIZE);
    }
    map = mmap(NULL, (size_t)(st.st_size ? st.st_size : PAGE_SIZE),
               PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (map == MAP_FAILED) { close(fd); return -1; }
    map_size = (size_t)(st.st_size ? st.st_size : PAGE_SIZE);
    hdr = (Header*)map;
    if (hdr->magic != MAGIC) { munmap(map, map_size); close(fd); return -1; }

    docs = calloc(MAX_DOCS, sizeof(Doc));
    /* Reconstruct doc entries from text region */
    uint32_t off = hdr->doc_text_off;
    for (uint32_t i = 0; i < hdr->num_docs && i < MAX_DOCS; i++) {
        if (off >= hdr->tri_off) break;
        uint8_t *start = map + off;
        uint8_t *nl = memchr(start, '\n', hdr->tri_off - off);
        if (!nl) break;
        uint16_t len = (uint16_t)(nl - start);
        docs[i].offset = off;
        docs[i].len = len;
        docs[i].dr = digital_root(start, len);
        docs[i].deleted = (len == 0xFFFF) ? 1 : 0;
        off += len + 1;  /* +1 for newline */
    }
    return 0;
}

static void db_close(void) {
    if (map) { munmap(map, map_size); map = NULL; }
    if (fd >= 0) { close(fd); fd = -1; }
    free(docs); docs = NULL;
}

/* ── Doc text access ── */
ACHT_INLINE const char *doc_text(uint32_t id) {
    if (id >= hdr->num_docs || docs[id].deleted) return NULL;
    return (const char*)(map + docs[id].offset);
}

/* ── Add document ── */
static uint32_t doc_add(const char *text) {
    int tlen = strlen(text);
    if (tlen <= 0 || tlen > 65535 || hdr->num_docs >= MAX_DOCS) return (uint32_t)-1;

    /* Find write position: after last doc */
    uint32_t off;
    if (hdr->num_docs == 0) {
        off = hdr->doc_text_off;
    } else {
        uint32_t last = hdr->num_docs - 1;
        off = docs[last].offset + docs[last].len + 1;
    }

    size_t need = off + (uint32_t)tlen + 1;
    if (need >= hdr->tri_off) {  /* would overlap trigram area */
        /* Shift trigram area up */
        uint32_t growth = (uint32_t)tlen + 1;
        size_t tri_start = hdr->tri_off;
        size_t tri_new = tri_start + growth;
        size_t total_need = tri_new + hdr->tri_size;
        if (grow(total_need) < 0) return (uint32_t)-1;
        /* Move trigram data */
        memmove(map + tri_new, map + tri_start, hdr->tri_size);
        hdr->tri_off = (uint32_t)tri_new;
    }
    if ((size_t)(off + tlen + 1) > map_size && grow((size_t)(off + tlen + 1)) < 0)
        return (uint32_t)-1;

    memcpy(map + off, text, (size_t)tlen);
    map[off + tlen] = '\n';

    uint32_t id = hdr->num_docs;
    docs[id].offset = off;
    docs[id].len = (uint16_t)tlen;
    docs[id].dr = digital_root((const uint8_t*)text, tlen);
    docs[id].deleted = 0;
    /* Compute bloom: OR of all trigram hashes (2 bits each) */
    {   uint64_t bf = 0;
        char low[4096]; int blen = tlen < 4096 ? tlen : 4095;
        for (int i = 0; i < blen; i++) low[i] = (char)tolower((uint8_t)text[i]);
        for (int i = 0; i <= blen - QLEN; i++) {
            uint64_t h = plumshash((const uint8_t*)(low + i), QLEN, 0x9E3779B9);
            bf |= (1ULL << (h & 63)) | (1ULL << ((h >> 6) & 63));
        }
        docs[id].bloom = bf;
    }

    /* Write doc offset table entry */
    uint64_t entry = ((uint64_t)off << 16) | (uint64_t)tlen;
    memcpy(map + PAGE_SIZE + id * 8, &entry, 8);

    hdr->num_docs++;
    hdr->tri_built = 0;  /* invalidate trigram index */
    return id;
}

/* ── Trigram index ── */

/* Varint encoding for posting list deltas */
static uint8_t *put_varint(uint8_t *p, uint32_t v) {
    while (v >= 0x80) { *p++ = (uint8_t)(v | 0x80); v >>= 7; }
    *p++ = (uint8_t)v;
    return p;
}

/* Build trigram inverted index: for each document, for each trigram,
 * append (doc_id, offset) as delta-encoded varint to posting list. */
static void tri_build(void) {
    uint32_t dir_size = N_BUCKETS * 4;
    /* Pass 1: count bytes needed per bucket */
    uint32_t *counts = calloc(N_BUCKETS, sizeof(uint32_t));
    for (uint32_t d = 0; d < hdr->num_docs; d++) {
        if (docs[d].deleted) continue;
        const char *text = doc_text(d);
        if (!text) continue;
        int tl = docs[d].len;
        /* Lowercase on stack */
        char buf[4096];
        int blen = tl < 4096 ? tl : 4095;
        for (int i = 0; i < blen; i++) buf[i] = (char)tolower((uint8_t)text[i]);

        for (int i = 0; i <= blen - QLEN; i++) {
            uint64_t h = plumshash((const uint8_t*)(buf + i), QLEN, 0x9E3779B9);
            uint32_t b = (uint32_t)(h % N_BUCKETS);
            /* Estimate: 1 fp byte + 2 varints ≈ 5 bytes */
            counts[b] += 5;
        }
    }

    /* Allocate index: dir_size + total data bytes */
    uint32_t total = dir_size;
    for (uint32_t b = 0; b < N_BUCKETS; b++) total += counts[b];

    /* Ensure space */
    uint32_t tri_start = hdr->tri_off;
    if (grow(tri_start + total) < 0) { free(counts); return; }

    /* Set up directory */
    uint32_t *dir = (uint32_t*)(map + tri_start);
    uint32_t data_off = dir_size;
    for (uint32_t b = 0; b < N_BUCKETS; b++) {
        dir[b] = data_off;
        data_off += counts[b];
    }
    hdr->tri_size = total;

    /* Reset counts for write pass */
    memset(counts, 0, N_BUCKETS * sizeof(uint32_t));

    /* Pass 2: fill buckets */
    for (uint32_t d = 0; d < hdr->num_docs; d++) {
        if (docs[d].deleted) continue;
        const char *text = doc_text(d);
        if (!text) continue;
        int tl = docs[d].len;
        char buf[4096];
        int blen = tl < 4096 ? tl : 4095;
        for (int i = 0; i < blen; i++) buf[i] = (char)tolower((uint8_t)text[i]);

        uint32_t prev = (d > 0 && !docs[d-1].deleted) ? d - 1 : (uint32_t)-1;
        for (int i = 0; i <= blen - QLEN; i++) {
            uint64_t h = plumshash((const uint8_t*)(buf + i), QLEN, 0x9E3779B9);
            uint32_t b = (uint32_t)(h % N_BUCKETS);
            uint8_t *dst = map + tri_start + dir[b] + counts[b];
            /* Fingerprint byte: high byte of trigram hash */
            *dst++ = (uint8_t)(h >> 24);  /* use bits 24-31 as fp */
            /* Encode (doc_id delta, offset) */
            uint32_t delta = (prev == (uint32_t)-1) ? d : d - prev;
            dst = put_varint(dst, delta);
            dst = put_varint(dst, (uint32_t)i);
            counts[b] = (uint32_t)(dst - (map + tri_start + dir[b]));
            prev = d;
        }
    }

    /* Rebuild directory using actual (not estimated) byte counts */
    {
        uint32_t data_off = dir_size;
        for (uint32_t b = 0; b < N_BUCKETS; b++) {
            uint32_t old_off = dir[b];
            uint32_t new_off = data_off;
            if (old_off != new_off && counts[b] > 0) {
                memmove(map + tri_start + new_off,
                        map + tri_start + old_off, counts[b]);
            }
            dir[b] = data_off;
            data_off += counts[b];
        }
        hdr->tri_size = data_off;
    }

    free(counts);
    hdr->tri_built = 1;
}

/* ── Posting list reader ── */
typedef struct {
    uint8_t *p;
    uint8_t *end;
    uint32_t prev_doc;
} PostingIter;

static void pi_init(PostingIter *pi, uint32_t bucket) {
    uint32_t *dir = (uint32_t*)(map + hdr->tri_off);
    uint32_t off = dir[bucket];
    uint32_t next = (bucket + 1 < N_BUCKETS) ? dir[bucket + 1] : hdr->tri_size;
    pi->p = map + hdr->tri_off + off;
    pi->end = map + hdr->tri_off + next;
    pi->prev_doc = (uint32_t)-1;
}

/* Read next (doc_id, offset) from posting list. Returns fp in *fp_out, 1 on success. */
static int pi_next(PostingIter *pi, uint32_t *doc, uint32_t *offs, uint8_t *fp_out) {
    if (pi->p >= pi->end) return 0;
    /* Read fingerprint byte */
    uint8_t fp = *pi->p++;
    if (fp_out) *fp_out = fp;
    /* Read varint delta */
    uint32_t delta = 0, shift = 0;
    while (pi->p < pi->end) {
        uint8_t b = *pi->p++;
        delta |= (b & 0x7F) << shift;
        if (!(b & 0x80)) break;
        shift += 7;
    }
    /* Read varint offset */
    uint32_t off = 0; shift = 0;
    while (pi->p < pi->end) {
        uint8_t b = *pi->p++;
        off |= (b & 0x7F) << shift;
        if (!(b & 0x80)) break;
        shift += 7;
    }
    pi->prev_doc = (pi->prev_doc == (uint32_t)-1) ? delta : pi->prev_doc + delta;
    *doc = pi->prev_doc;
    *offs = off;
    return 1;
}

/* ── Myers bit-parallel edit distance ── */
ACHT_HOT static int myers_ed(const char *a, int la, const char *b, int lb, int max_k) {
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

/* Damerau-Levenshtein fallback */
static int dl_ed(const char *a, int la, const char *b, int lb, int max_k) {
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

/* Substring edit distance: checks near a specific offset, or full doc if off==0 */
static int substr_ed(const char *q, int ql, const char *t, int tl, int max_k,
                      uint32_t off) {
    int best = max_k + 1;
    int lo = 0, hi = tl;
    if (off > 0) {
        lo = (int)off - ql - max_k; if (lo < 0) lo = 0;
        hi = (int)off + ql + max_k; if (hi > tl) hi = tl;
    }
    for (int start = lo; start < hi; start++) {
        for (int wlen = ql - max_k; wlen <= ql + max_k; wlen++) {
            if (wlen < 1) continue;
            if (start + wlen > tl) break;
            int ed = myers_ed(q, ql, t + start, wlen, max_k);
            if (ed < 0 || ed > max_k) ed = dl_ed(q, ql, t + start, wlen, max_k);
            if (ed >= 0 && ed < best) best = ed;
            if (best == 0) return 0;
        }
    }
    return best;
}

/* ── qsort comparator ── */
static int scmp(const void *va, const void *vb) {
    int ea = ((const struct scored*)va)->ed;
    int eb = ((const struct scored*)vb)->ed;
    return (ea > eb) - (ea < eb);
}

/* ═══════════════════════════════════════════════════════
 * SEARCH
 * ═══════════════════════════════════════════════════════ */

static int ffdb_search(const char *query, int max_k, uint32_t *results, int max_results) {
    if (!hdr->tri_built) tri_build();
    if (hdr->num_docs == 0) return 0;

    int ql = strlen(query);
    if (ql > MYERS_MAX) ql = MYERS_MAX;

    /* Lowercase query */
    char qlow[MYERS_MAX + 1];
    int qllen = ql < MYERS_MAX ? ql : MYERS_MAX;
    for (int i = 0; i < qllen; i++) qlow[i] = (char)tolower((uint8_t)query[i]);
    qlow[qllen] = 0;

    int nscored = 0;
    struct scored scored_buf[MAX_SCORED];

    if (ql < QLEN) {
        /* Too short for trigrams — linear scan with digital root pre-filter */
        for (uint32_t d = 0; d < hdr->num_docs && nscored < MAX_SCORED; d++) {
            if (docs[d].deleted) continue;
            /* DR not used for substring fuzzy matching */
            const char *text = doc_text(d);
            if (!text) continue;
            int tl = docs[d].len;
            int ed = substr_ed(qlow, qllen, text, tl, max_k, 0);  /* full scan */
            if (ed <= max_k) {
                scored_buf[nscored].id = d;
                scored_buf[nscored].ed = ed;
                nscored++;
            }
        }
        qsort(scored_buf, nscored, sizeof(scored_buf[0]), scmp);
        int out = 0;
        for (int i = 0; i < nscored && out < max_results; i++)
            results[out++] = scored_buf[i].id;
        return out;
    }

    /* Extract query trigrams, compute query bloom */
    uint32_t q_tri[MYERS_MAX];
    uint64_t q_bloom = 0;
    int nqt = 0;
    for (int i = 0; i <= qllen - QLEN; i++) {
        uint64_t h = plumshash((const uint8_t*)(qlow + i), QLEN, 0x9E3779B9);
        q_bloom |= (1ULL << (h & 63)) | (1ULL << ((h >> 6) & 63));
        q_tri[nqt++] = (uint32_t)(h % N_BUCKETS);
    }
    /* Dedup and sort query trigram buckets for two-pointer intersection */
    for (int i = 1; i < nqt; i++) {
        uint32_t x = q_tri[i]; int k = i - 1;
        while (k >= 0 && q_tri[k] > x) { q_tri[k+1] = q_tri[k]; k--; }
        q_tri[k+1] = x;
    }
    /* Dedup q_tri */
    { int u = 1;
      for (int i = 1; i < nqt; i++)
          if (q_tri[i] != q_tri[u-1]) q_tri[u++] = q_tri[i];
      nqt = u; }
    int min_ov;
    min_ov = nqt - 3 * max_k; if (min_ov < 1) min_ov = 1;

    /* Build query fingerprint set (256 bits = 32 bytes) */
    uint8_t q_fp_set[32] = {0};
    for (int i = 0; i <= qllen - QLEN; i++) {
        uint64_t h = plumshash((const uint8_t*)(qlow + i), QLEN, 0x9E3779B9);
        uint8_t fp = (uint8_t)(h >> 24);
        q_fp_set[fp >> 3] |= (uint8_t)(1 << (fp & 7));
    }

    /* Iterate candidates from all non-empty query trigram buckets.
     * Multi-bucket ensures we don't miss docs whose postings aren't
     * in the single rarest bucket. seen[] deduplicates across buckets. */
    uint8_t *seen = calloc((hdr->num_docs + 7) / 8, 1);
    for (int bi = 0; bi < nqt && nscored < MAX_SCORED; bi++) {
        uint32_t bucket = q_tri[bi];
        uint32_t *dir = (uint32_t*)(map + hdr->tri_off);
        uint32_t boff = dir[bucket];
        uint32_t bnext = (bucket + 1 < N_BUCKETS) ? dir[bucket+1] : hdr->tri_size;
        if (bnext <= boff) continue;  /* empty bucket */

        PostingIter pi;
        pi_init(&pi, bucket);
        uint32_t doc, offs;
        uint8_t fp;

    while (pi_next(&pi, &doc, &offs, &fp) && nscored < MAX_SCORED) {
        if (doc >= hdr->num_docs || docs[doc].deleted) continue;
        if (seen[doc >> 3] & (1 << (doc & 7))) continue;

        /* Layer 1: bloom pre-filter — fast negative test */
        uint64_t match_bloom = docs[doc].bloom & q_bloom;
        if ((int)__builtin_popcountll(match_bloom) < min_ov * 2) continue;

        const char *text = doc_text(doc);
        if (!text) continue;
        int tl = docs[doc].len;
        if (tl < qllen - max_k) continue;

        /* Layer 2: trigram intersection — pre-compute doc trigram buckets
         * once, then use sorted two-pointer merge for O(n+m) intersection. */
        int tri_match = 0;
        char dlow[4096];
        int dlen = tl < 4096 ? tl : 4095;
        for (int j = 0; j < dlen; j++) dlow[j] = (char)tolower((uint8_t)text[j]);

        /* Pre-compute document trigram bucket IDs */
        uint32_t dtri[4096];
        int ndt = 0;
        for (int j = 0; j <= dlen - QLEN; j++) {
            uint64_t h = plumshash((const uint8_t*)(dlow + j), QLEN, 0x9E3779B9);
            dtri[ndt++] = (uint32_t)(h % N_BUCKETS);
        }
        /* Sort for two-pointer intersection */
        for (int i = 1; i < ndt; i++) {
            uint32_t x = dtri[i]; int k = i - 1;
            while (k >= 0 && dtri[k] > x) { dtri[k+1] = dtri[k]; k--; }
            dtri[k+1] = x;
        }
        /* Dedup */
        { int u = 1;
          for (int i = 1; i < ndt; i++)
              if (dtri[i] != dtri[u-1]) dtri[u++] = dtri[i];
          ndt = u; }

        /* Two-pointer intersection: q_tri[] is already bucket IDs (no hash needed) */
        { int qi = 0, di = 0;
          while (qi < nqt && di < ndt) {
              if (q_tri[qi] < dtri[di]) qi++;
              else if (dtri[di] < q_tri[qi]) di++;
              else { tri_match++; qi++; di++; }
          }
        }
        if (tri_match < min_ov) { /* fprintf(stderr, "skip doc=%u tri=%d<%d\n", doc, tri_match, min_ov); */ continue; }

        /* Layer 3: edit distance verification */
        int ed = substr_ed(qlow, qllen, text, tl, max_k, offs);
        if (ed <= max_k) {
            scored_buf[nscored].id = doc;
            scored_buf[nscored].ed = ed;
            nscored++;
            seen[doc >> 3] |= (1 << (doc & 7));
        }
    }

    } /* end for each bucket */

    free(seen);
    qsort(scored_buf, nscored, sizeof(scored_buf[0]), scmp);
    int out = 0;
    for (int i = 0; i < nscored && out < max_results; i++)
        results[out++] = scored_buf[i].id;
    return out;
}

/* ── Substring search (uses trigram index + strcasestr) ── */
static int ffdb_find(const char *needle, uint32_t *results, int max_results) {
    if (!hdr->tri_built) tri_build();
    int nl = strlen(needle);

    if (nl < QLEN) {
        int found = 0;
        for (uint32_t d = 0; d < hdr->num_docs && found < max_results; d++) {
            if (docs[d].deleted) continue;
            const char *text = doc_text(d);
            if (text && strcasestr(text, needle)) results[found++] = d;
        }
        return found;
    }

    char nlow[256]; int nllen = nl < 256 ? nl : 255;
    for (int i = 0; i < nllen; i++) nlow[i] = (char)tolower((uint8_t)needle[i]);

    /* Find rarest query trigram bucket */
    int best_b = -1; uint32_t best_sz = 0xFFFFFFFF;
    for (int i = 0; i <= nllen - QLEN; i++) {
        uint64_t h = plumshash((const uint8_t*)(nlow + i), QLEN, 0x9E3779B9);
        uint32_t b = (uint32_t)(h % N_BUCKETS);
        uint32_t *dir = (uint32_t*)(map + hdr->tri_off);
        uint32_t sz = (b + 1 < N_BUCKETS ? dir[b+1] : hdr->tri_size) - dir[b];
        if (sz > 0 && sz < best_sz) { best_sz = sz; best_b = (int)b; }
    }
    if (best_b < 0) return 0;

    int found = 0;
    uint8_t *seen = calloc((hdr->num_docs + 7) / 8, 1);
    PostingIter pi;
    pi_init(&pi, (uint32_t)best_b);
    uint32_t doc, offs;
    uint8_t fp;

    while (pi_next(&pi, &doc, &offs, &fp) && found < max_results) {
        if (doc >= hdr->num_docs || docs[doc].deleted) continue;
        if (seen[doc >> 3] & (1 << (doc & 7))) continue;
        const char *text = doc_text(doc);
        if (text && strcasestr(text, needle)) {
            results[found++] = doc;
            seen[doc >> 3] |= (1 << (doc & 7));
        }
    }
    free(seen);
    return found;
}

/* ═══════════════════════════════════════════════════════
 * COMMANDS
 * ═══════════════════════════════════════════════════════ */

static void cmd_add(int argc, char **argv) {
    if (argc < 2) { printf("Usage: add TEXT\n"); return; }
    char text[4096]; text[0] = 0;
    for (int i = 1; i < argc; i++) {
        if (i > 1) strcat(text, " ");
        strcat(text, argv[i]);
    }
    uint32_t id = doc_add(text);
    if (id == (uint32_t)-1) { printf("Failed\n"); return; }
    printf("ok doc=%u dr=%u\n", id, docs[id].dr);
}

static void cmd_search(int argc, char **argv) {
    if (argc < 2) { printf("Usage: search QUERY [K]\n"); return; }
    char query[256]; query[0] = 0;
    for (int i = 1; i < argc; i++) {
        if (i > 1 && !isdigit(argv[i][0]) && argv[i][0] != '-') strcat(query, " ");
        if (i > 1 && isdigit(argv[i][0])) break;
        strcat(query, argv[i]);
    }
    int max_k = 2;
    if (argc >= 3 && isdigit(argv[argc-1][0])) max_k = atoi(argv[argc-1]);

    if (!hdr->tri_built) {
        fprintf(stderr, "Building index... "); fflush(stderr);
        clock_t t = clock(); tri_build();
        fprintf(stderr, "done (%.3fs)\n", (double)(clock()-t)/CLOCKS_PER_SEC);
    }

    uint32_t results[MAX_RES];
    int n = ffdb_search(query, max_k, results, MAX_RES);
    for (int i = 0; i < n; i++) {
        const char *text = doc_text(results[i]);
        int ed = 0;
        if (text) {
            char qlow[256], tlow[256];
            int ql = strlen(query); if (ql > 255) ql = 255;
            int tl = docs[results[i]].len; if (tl > 255) tl = 255;
            for (int j = 0; j < ql; j++) qlow[j] = (char)tolower((uint8_t)query[j]); qlow[ql] = 0;
            for (int j = 0; j < tl; j++) tlow[j] = (char)tolower((uint8_t)text[j]); tlow[tl] = 0;
            ed = substr_ed(qlow, ql, tlow, tl, max_k, 0);
        }
        printf("ed=%d [%u] ", ed, results[i]);
        if (text) { int tl = docs[results[i]].len; if (tl > 80) tl = 80;
            printf("%.*s%s\n", tl, text, docs[results[i]].len > 80 ? "..." : ""); }
        else printf("\n");
    }
    printf("%d results\n", n);
}

static void cmd_find(int argc, char **argv) {
    if (argc < 2) { printf("Usage: find SUBSTRING\n"); return; }
    char needle[256]; needle[0] = 0;
    for (int i = 1; i < argc; i++) {
        if (i > 1) strcat(needle, " "); strcat(needle, argv[i]); }
    if (!hdr->tri_built) tri_build();
    uint32_t results[MAX_RES];
    int n = ffdb_find(needle, results, MAX_RES);
    for (int i = 0; i < n; i++) {
        const char *text = doc_text(results[i]);
        printf("[%u] ", results[i]);
        if (text) { int tl = docs[results[i]].len; if (tl > 80) tl = 80;
            printf("%.*s%s\n", tl, text, docs[results[i]].len > 80 ? "..." : ""); }
        else printf("\n");
    }
    printf("%d results\n", n);
}

static void cmd_count(void) {
    uint32_t n = 0;
    for (uint32_t i = 0; i < hdr->num_docs; i++)
        if (!docs[i].deleted) n++;
    printf("%u docs (%u total, %u deleted)\n", n, hdr->num_docs, hdr->num_docs - n);
}

static void cmd_bench(int argc, char **argv) {
    int N = (argc > 1) ? atoi(argv[1]) : 10000;
    if (N < 100) N = 100; if (N > 200000) N = 200000;

    if (hdr->num_docs < (uint32_t)N) {
        fprintf(stderr, "Generating %d documents... ", N);
        const char *words[] = {"the","quick","brown","fox","jumps","over","lazy","dog",
            "amsterdam","rotterdam","utrecht","database","search","fuzzy","index",
            "hash","query","document","text","engine","fractal","digital","root"};
        int nw = 23;
        for (int i = 0; i < N; i++) {
            char buf[256]; buf[0] = 0;
            int nwords = 5 + (i % 15);
            for (int w = 0; w < nwords; w++) {
                if (w > 0) strcat(buf, " ");
                strcat(buf, words[(i * 7 + w * 13) % nw]);
            }
            doc_add(buf);
        }
        fprintf(stderr, "done\n");
    }

    if (!hdr->tri_built) {
        fprintf(stderr, "Building trigram index... "); fflush(stderr);
        clock_t t = clock(); tri_build();
        fprintf(stderr, "done (%.3fs, %u bytes)\n",
            (double)(clock()-t)/CLOCKS_PER_SEC, hdr->tri_size);
    }

    printf("╔════════════════════════════════════════════════════════╗\n");
    printf("║  ffdb v%d BENCH  —  %u docs, %u KB index         ║\n",
        VERSION, hdr->num_docs, hdr->tri_size / 1024);
    printf("╠════════════════════════════════════════════════════════╣\n");

    {   const char *q = "amsterdm"; int iters = 200;
        clock_t t = clock(); volatile int s = 0;
        for (int i = 0; i < iters; i++) {
            uint32_t found[MAX_RES];
            s += ffdb_search(q, 2, found, MAX_RES);
        }
        double dt = (double)(clock()-t)/CLOCKS_PER_SEC;
        printf("║ Fuzzy search (ed≤2)   %8d ops: %8.3fs → %9.1f µs/op ║\n",
            iters, dt, dt*1e6/iters);
    }

    {   const char *q = "dam"; int iters = 1000;
        clock_t t = clock(); volatile int s = 0;
        for (int i = 0; i < iters; i++) {
            uint32_t found[MAX_RES];
            s += ffdb_find(q, found, MAX_RES);
        }
        double dt = (double)(clock()-t)/CLOCKS_PER_SEC;
        printf("║ Substring (trigram)   %8d ops: %8.3fs → %9.1f µs/op ║\n",
            iters, dt, dt*1e6/iters);
    }

    {   const char *q = "dam"; int iters = 100;
        clock_t t = clock(); volatile int s = 0;
        for (int i = 0; i < iters; i++) {
            for (uint32_t d = 0; d < hdr->num_docs; d++) {
                const char *text = doc_text(d);
                if (text && strcasestr(text, q)) s++;
            }
        }
        double dt = (double)(clock()-t)/CLOCKS_PER_SEC;
        printf("║ Substring (linear)    %8d ops: %8.3fs → %9.1f µs/op ║\n",
            iters, dt, dt*1e6/iters);
    }

    printf("╠════════════════════════════════════════════════════════╣\n");
    printf("║  STORAGE: %zu KB mmap, %u docs                    ║\n",
        map_size/1024, hdr->num_docs);
    printf("╚════════════════════════════════════════════════════════╝\n");
}

/* ── REPL ── */
static void cmd_serve(void) {
    char line[4096];
    printf("ffdb> "); fflush(stdout);
    while (fgets(line, sizeof(line), stdin)) {
        int len = strlen(line);
        while (len>0 && (line[len-1]=='\n'||line[len-1]=='\r')) line[--len]=0;
        if (len==0) { printf("ffdb> "); fflush(stdout); continue; }
        char *tokens[32]; int nt=0;
        char *tok = strtok(line, " ");
        while (tok && nt<32) { tokens[nt++]=tok; tok=strtok(NULL," "); }
        if (nt==0) continue;
        if (!strcmp(tokens[0],"exit")||!strcmp(tokens[0],"quit")) break;
        else if (!strcmp(tokens[0],"add"))    cmd_add(nt, tokens);
        else if (!strcmp(tokens[0],"search")) cmd_search(nt, tokens);
        else if (!strcmp(tokens[0],"find"))   cmd_find(nt, tokens);
        else if (!strcmp(tokens[0],"count"))  cmd_count();
        else if (!strcmp(tokens[0],"bench"))  cmd_bench(nt, tokens);
        else printf("? %s\n", tokens[0]);
        fflush(stdout);
        printf("ffdb> "); fflush(stdout);
    }
}

int main(int argc, char **argv) {
    if (argc < 2) {
        printf("ffdb v%d — fractal fuzzy text search database\n\n", VERSION);
        printf("  add TEXT              Add document\n");
        printf("  search QUERY [K]      Fuzzy search (ed ≤ K, default 2)\n");
        printf("  find SUBSTRING        Substring search\n");
        printf("  count                 Document count\n");
        printf("  bench [N]             Benchmark\n");
        printf("  serve                 Interactive REPL\n");
        return 0;
    }
    if (db_open(DB_FILE) < 0) { fprintf(stderr, "Cannot open %s\n", DB_FILE); return 1; }
    const char *cmd = argv[1];
    if (!strcmp(cmd,"add"))    cmd_add(argc-1, argv+1);
    else if (!strcmp(cmd,"search")) cmd_search(argc-1, argv+1);
    else if (!strcmp(cmd,"find"))   cmd_find(argc-1, argv+1);
    else if (!strcmp(cmd,"count"))  cmd_count();
    else if (!strcmp(cmd,"bench"))  cmd_bench(argc-1, argv+1);
    else if (!strcmp(cmd,"serve"))  cmd_serve();
    else printf("? %s\n", cmd);
    db_close();
    return 0;
}
