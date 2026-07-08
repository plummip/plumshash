/*
 * fuzzydb.c — Embedded fuzzy full-text search database.
 *
 * Architecture:
 *   - mmap'd file: header + doc store + trigram inverted index
 *   - Documents: length-prefixed text blobs (4B len + text)
 *   - Inverted trigram index: trigram → sorted list of doc_ids
 *   - Search: extract trigrams → intersect candidate sets → verify ed → rank
 *
 * Commands:
 *   add TEXT                  Add document, returns doc_id
 *   search QUERY [K]          Fuzzy search (edit distance ≤ K, default 2)
 *   find SUBSTRING            Substring search
 *   delete ID                 Remove document
 *   count                     Document count
 *   bench [N]                 Benchmark with N random queries
 *   serve                     Interactive REPL
 *
 * Compile: gcc -O3 -march=armv8-a -o fuzzydb fuzzydb.c -I.
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

/* ── Compiler hints ── */
#if defined(__GNUC__) || defined(__clang__)
#define LIKELY(x)   __builtin_expect(!!(x), 1)
#define UNLIKELY(x) __builtin_expect(!!(x), 0)
#define ACHT_HOT    __attribute__((hot))
#define ACHT_INLINE __attribute__((always_inline)) static inline
#else
#define LIKELY(x)   (x)
#define UNLIKELY(x) (x)
#define ACHT_HOT
#define ACHT_INLINE static inline
#endif

/* ── Constants ── */
#define PAGE_SIZE       4096
#define MAGIC           0x46555A44  /* "FUZD" */
#define VERSION         1
#define DB_FILE         "data.fuzzydb"
#define MAX_DOCS        1000000
#define MAX_RES         200
#define N_TRIGRAM_BUCKETS 1009  /* prime */
#define FZY_MAX_SCORED  512
#define FZY_MYERS_CUTOFF 62  /* max safe 64-bit Myers (63 bits + overflow guard) */

/* ── On-disk header (page 0) ── */
typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint32_t version;
    uint32_t num_docs;
    uint32_t doc_store_offset;   /* byte offset to doc store */
    uint32_t doc_store_size;     /* allocated bytes for doc store */
    uint32_t tri_offset;         /* byte offset to trigram index */
    uint32_t tri_size;           /* allocated bytes for trigram index */
    uint32_t tri_ntrigrams;      /* total trigram entries */
    uint8_t  _pad[PAGE_SIZE - 32];
} DiskHeader;

/* ── In-memory doc entry ── */
typedef struct {
    uint32_t offset;   /* byte offset in mmap */
    uint32_t len;      /* text length */
    uint8_t  deleted;
} DocEntry;

/* ── Fuzzy search result ── */
struct fzy_scored { uint32_t doc_id; int ed; };

/* ── Global state ── */
static uint8_t    *db_map = NULL;
static size_t      db_size = 0;
static int         db_fd = -1;
static DiskHeader *disk_hdr = NULL;
static DocEntry   *docs = NULL;      /* heap-allocated doc index */
static uint32_t    doc_cap = 0;

/* ── File management ── */
static int db_grow(size_t new_size) {
    if (new_size <= db_size) return 0;
    new_size = ((new_size + PAGE_SIZE - 1) / PAGE_SIZE) * PAGE_SIZE;
    if (ftruncate(db_fd, (off_t)new_size) < 0) return -1;
    if (db_map) munmap(db_map, db_size);
    db_map = mmap(NULL, new_size, PROT_READ | PROT_WRITE, MAP_SHARED, db_fd, 0);
    if (db_map == MAP_FAILED) return -1;
    db_size = new_size;
    disk_hdr = (DiskHeader*)db_map;
    return 0;
}

static int db_open(const char *path) {
    db_fd = open(path, O_RDWR | O_CREAT, 0644);
    if (db_fd < 0) return -1;

    struct stat st; fstat(db_fd, &st);
    if (st.st_size == 0) {
        DiskHeader h; memset(&h, 0, sizeof(h));
        h.magic = MAGIC; h.version = VERSION;
        h.doc_store_offset = PAGE_SIZE;
        h.doc_store_size = 0;
        h.tri_offset = PAGE_SIZE + 64 * 1024 * 1024;  /* 64MB gap for docs */
        h.tri_size = 0;
        write(db_fd, &h, sizeof(h));
        ftruncate(db_fd, PAGE_SIZE);
    }

    db_map = mmap(NULL, (size_t)(st.st_size ? st.st_size : PAGE_SIZE),
                  PROT_READ | PROT_WRITE, MAP_SHARED, db_fd, 0);
    if (db_map == MAP_FAILED) { close(db_fd); return -1; }
    db_size = (size_t)(st.st_size ? st.st_size : PAGE_SIZE);
    disk_hdr = (DiskHeader*)db_map;

    if (disk_hdr->magic != MAGIC) { munmap(db_map, db_size); close(db_fd); return -1; }

    /* Load doc entries from mmap */
    doc_cap = MAX_DOCS;
    docs = calloc(doc_cap, sizeof(DocEntry));
    /* Reconstruct doc entries by scanning doc store */
    uint32_t off = disk_hdr->doc_store_offset;
    for (uint32_t i = 0; i < disk_hdr->num_docs && i < doc_cap; i++) {
        if (off + 4 > disk_hdr->doc_store_offset + disk_hdr->doc_store_size) break;
        uint32_t len; memcpy(&len, db_map + off, 4);
        docs[i].offset = off;
        docs[i].len = len;
        docs[i].deleted = (len & 0x80000000) ? 1 : 0;
        off += 4 + (len & 0x7FFFFFFF);
    }
    return 0;
}

static void db_close(void) {
    if (db_map) { munmap(db_map, db_size); db_map = NULL; }
    if (db_fd >= 0) { close(db_fd); db_fd = -1; }
    free(docs); docs = NULL;
}

/* ── Document text access ── */
ACHT_INLINE const char *doc_text(uint32_t doc_id) {
    if (doc_id >= disk_hdr->num_docs || docs[doc_id].deleted) return NULL;
    uint32_t off = docs[doc_id].offset;
    /* unused — kept for clarity */
    (void)(docs[doc_id].len & 0x7FFFFFFF);
    return (const char*)(db_map + off + 4);
}

ACHT_INLINE int doc_len(uint32_t doc_id) {
    if (doc_id >= disk_hdr->num_docs) return 0;
    return (int)(docs[doc_id].len & 0x7FFFFFFF);
}

/* ── Add document ── */
static uint32_t doc_add(const char *text) {
    int tlen = strlen(text);
    if (tlen <= 0) return (uint32_t)-1;
    if (disk_hdr->num_docs >= doc_cap) return (uint32_t)-1;

    uint32_t need = disk_hdr->doc_store_size + 4 + (uint32_t)tlen;
    size_t end = disk_hdr->doc_store_offset + need;
    if (end > db_size) db_grow(end);

    uint32_t off = disk_hdr->doc_store_offset + disk_hdr->doc_store_size;
    uint32_t len = (uint32_t)tlen;
    memcpy(db_map + off, &len, 4);
    memcpy(db_map + off + 4, text, (size_t)tlen);

    uint32_t id = disk_hdr->num_docs;
    docs[id].offset = off;
    docs[id].len = len;
    docs[id].deleted = 0;

    disk_hdr->num_docs++;
    disk_hdr->doc_store_size = need;
    return id;
}

/* ── Trigram extraction ── */
ACHT_INLINE uint32_t tri_hash(const uint8_t *s) {
    return (uint32_t)(plumshash(s, 3, 0x9E3779B9) % N_TRIGRAM_BUCKETS);
}

static int tri_extract(const char *s, uint32_t *out) {
    int len = strlen(s);
    if (len < 3) return 0;
    const uint8_t *u = (const uint8_t*)s;
    int n = 0;
    for (int i = 0; i <= len - 3; i++)
        out[n++] = tri_hash(u + i);
    return n;
}

/* ── Trigram inverted index ── */

/* Allocate trigram index: directory (N_TRIGRAM_BUCKETS * 4 bytes) + data area */
static int tri_init(void) {
    if (disk_hdr->tri_size > 0) return 0;  /* already initialized */

    uint32_t dir_size = N_TRIGRAM_BUCKETS * 4;
    uint32_t data_size = 64 * PAGE_SIZE;  /* initial data area */
    disk_hdr->tri_size = dir_size + data_size;

    size_t need = disk_hdr->tri_offset + disk_hdr->tri_size;
    if (need > db_size && db_grow(need) < 0) return -1;

    /* Initialize directory: each entry points to end of previous data */
    uint32_t *dir = (uint32_t*)(db_map + disk_hdr->tri_offset);
    for (uint32_t i = 0; i < N_TRIGRAM_BUCKETS; i++)
        dir[i] = dir_size;  /* all buckets start empty at data area base */

    return 0;
}

/* Rebuild entire trigram index from all documents */
static int tri_rebuild(void) {
    tri_init();
    if (disk_hdr->tri_size == 0) return -1;

    uint32_t dir_size = N_TRIGRAM_BUCKETS * 4;
    (void)(disk_hdr->tri_size - dir_size);  /* data_max — reserved for future safety checks */

    /* Pass 1: count trigrams per bucket */
    uint32_t *counts = calloc(N_TRIGRAM_BUCKETS, sizeof(uint32_t));
    uint32_t total = 0;
    for (uint32_t d = 0; d < disk_hdr->num_docs; d++) {
        const char *text = doc_text(d);
        if (!text) continue;
        /* Lowercase */
        char buf[4096];
        int tl = strlen(text); if (tl > 4095) tl = 4095;
        for (int i = 0; i < tl; i++) buf[i] = (char)tolower((uint8_t)text[i]);
        buf[tl] = 0;

        uint32_t tri[4096];
        int nt = tri_extract(buf, tri);
        for (int j = 0; j < nt; j++) { counts[tri[j]]++; total++; }
    }

    /* Ensure enough space */
    uint32_t needed = dir_size + total * 4;
    if (needed > disk_hdr->tri_size) {
        /* Grow trigram area */
        size_t new_end = disk_hdr->tri_offset + needed;
        if (new_end > db_size) db_grow(new_end);
        disk_hdr->tri_size = needed;
    }

    /* Set up directory offsets */
    uint32_t *dir = (uint32_t*)(db_map + disk_hdr->tri_offset);
    uint32_t data_off = dir_size;
    for (uint32_t t = 0; t < N_TRIGRAM_BUCKETS; t++) {
        dir[t] = data_off;
        data_off += counts[t] * 4;
    }

    /* Pass 2: fill buckets */
    uint32_t *filled = calloc(N_TRIGRAM_BUCKETS, sizeof(uint32_t));
    for (uint32_t d = 0; d < disk_hdr->num_docs; d++) {
        const char *text = doc_text(d);
        if (!text) continue;
        char buf[4096];
        int tl = strlen(text); if (tl > 4095) tl = 4095;
        for (int i = 0; i < tl; i++) buf[i] = (char)tolower((uint8_t)text[i]);
        buf[tl] = 0;

        uint32_t tri[4096];
        int nt = tri_extract(buf, tri);
        for (int j = 0; j < nt; j++) {
            uint32_t t = tri[j];
            if (filled[t] < counts[t]) {
                uint32_t *bucket = (uint32_t*)(db_map + disk_hdr->tri_offset + dir[t]);
                bucket[filled[t]++] = d;
            }
        }
    }
    disk_hdr->tri_ntrigrams = total;
    free(counts); free(filled);
    return 0;
}

/* Get bucket for trigram hash t */
static uint32_t *tri_bucket(uint32_t t, uint32_t *count) {
    if (t >= N_TRIGRAM_BUCKETS || disk_hdr->tri_size == 0) {
        *count = 0; return NULL;
    }
    uint32_t *dir = (uint32_t*)(db_map + disk_hdr->tri_offset);
    uint32_t off = dir[t];
    uint32_t next = (t + 1 < N_TRIGRAM_BUCKETS) ? dir[t + 1] : disk_hdr->tri_size;
    *count = (next - off) / 4;
    return (uint32_t*)(db_map + disk_hdr->tri_offset + off);
}

/* ── Myers bit-parallel edit distance ── */
ACHT_HOT static int fzy_myers(const char *a, int la, const char *b, int lb, int max_k) {
    if (la > FZY_MYERS_CUTOFF || lb > FZY_MYERS_CUTOFF) return -1;
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
static int fzy_damerau(const char *a, int la, const char *b, int lb, int max_k) {
    if (la > 60) la = 60; if (lb > 60) lb = 60;
    int ld = la - lb; if (ld < 0) ld = -ld;
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

/* ── Substring-aware edit distance ── */
static int fzy_substring_ed(const char *query, int ql, const char *text, int tl, int max_k) {
    if (ql == 0) return 0;
    int best = max_k + 1;
    /* Slide exact-sized windows across text. For each start, try window lengths
     * from ql-max_k to ql+max_k. Myers handles length differences automatically. */
    for (int start = 0; start < tl; start++) {
        for (int wlen = ql - max_k; wlen <= ql + max_k; wlen++) {
            if (wlen < 1) continue;
            int end = start + wlen;
            if (end > tl) break;
            int ed = fzy_myers(query, ql, text + start, wlen, max_k);
            if (ed == -1) ed = fzy_damerau(query, ql, text + start, wlen, max_k);
            if (ed >= 0 && ed < best) best = ed;
            if (best == 0) return 0;
        }
    }
    return best;
}

/* ── Sort + dedup trigrams ── */
static int tri_sort_dedup(uint32_t *a, int n) {
    if (n <= 1) return n;
    for (int i = 1; i < n; i++) {
        uint32_t x = a[i]; int j = i - 1;
        while (j >= 0 && a[j] > x) { a[j+1] = a[j]; j--; }
        a[j+1] = x;
    }
    int out = 1;
    for (int i = 1; i < n; i++)
        if (a[i] != a[out-1]) a[out++] = a[i];
    return out;
}

/* ── Two-pointer intersection of sorted arrays ── */
static int tri_inter(const uint32_t *a, int na, const uint32_t *b, int nb) {
    int c = 0, i = 0, j = 0;
    while (i < na && j < nb) {
        if (a[i] < b[j]) i++;
        else if (b[j] < a[i]) j++;
        else { c++; i++; j++; }
    }
    return c;
}

/* ── qsort comparator ── */
static int scored_cmp(const void *va, const void *vb) {
    int ea = ((const struct fzy_scored*)va)->ed;
    int eb = ((const struct fzy_scored*)vb)->ed;
    return (ea > eb) - (ea < eb);
}

/* ═══════════════════════════════════════════════════════
 * FUZZY SEARCH
 * ═══════════════════════════════════════════════════════ */

static int fzy_search(const char *query, int max_k, uint32_t *results, int max_results) {
    if (disk_hdr->tri_ntrigrams == 0) tri_rebuild();
    if (disk_hdr->num_docs == 0) return 0;

    /* Lowercase query */
    char qlow[256];
    int ql = strlen(query); if (ql > 255) ql = 255;
    for (int i = 0; i < ql; i++) qlow[i] = (char)tolower((uint8_t)query[i]);
    qlow[ql] = 0;

    /* Extract + sort + dedup trigrams */
    uint32_t pt[256]; int npt = tri_extract(qlow, pt);
    if (npt == 0) {
        /* Query too short for trigrams — linear scan with ed */
        struct fzy_scored scored_buf[FZY_MAX_SCORED];
        int nscored = 0;
        for (uint32_t d = 0; d < disk_hdr->num_docs && nscored < FZY_MAX_SCORED; d++) {
            const char *text = doc_text(d);
            if (!text) continue;
            int tl = doc_len(d); if (tl > 255) tl = 255;
            char tlow[256];
            for (int i = 0; i < tl; i++) tlow[i] = (char)tolower((uint8_t)text[i]);
            tlow[tl] = 0;
            int ed = fzy_substring_ed(qlow, ql, tlow, tl, max_k);
            if (ed <= max_k) {
                scored_buf[nscored].doc_id = d;
                scored_buf[nscored].ed = ed;
                nscored++;
            }
        }
        qsort(scored_buf, nscored, sizeof(scored_buf[0]), scored_cmp);
        int out = 0;
        for (int i = 0; i < nscored && out < max_results; i++)
            results[out++] = scored_buf[i].doc_id;
        return out;
    }

    npt = tri_sort_dedup(pt, npt);
    int min_ov = npt - 3 * max_k; if (min_ov < 1) min_ov = 1;

    /* Find smallest bucket */
    int best_t = -1; uint32_t best_n = 0xFFFFFFFF;
    for (int i = 0; i < npt; i++) {
        uint32_t cnt; tri_bucket(pt[i], &cnt);
        if (cnt > 0 && cnt < best_n) { best_n = cnt; best_t = (int)pt[i]; }
    }
    if (best_t < 0) return 0;

    uint32_t nc, *cand = tri_bucket((uint32_t)best_t, &nc);
    if (!cand || nc > 100000) nc = 100000;

    /* seen tracking */
    uint8_t seen_static[4096];
    uint8_t *seen;
    int seen_alloc = 0;
    if (nc <= sizeof(seen_static)) {
        seen = seen_static; memset(seen, 0, nc);
    } else {
        seen = calloc(nc, 1); seen_alloc = 1;
    }

    struct fzy_scored scored_buf[FZY_MAX_SCORED];
    int nscored = 0;

    for (uint32_t i = 0; i < nc && nscored < FZY_MAX_SCORED; i++) {
        if (seen[i]) continue;
        uint32_t d = cand[i];
        const char *text = doc_text(d);
        if (!text) continue;

        char tlow[256];
        int tl = doc_len(d); if (tl > 255) tl = 255;
        for (int j = 0; j < tl; j++) tlow[j] = (char)tolower((uint8_t)text[j]);
        tlow[tl] = 0;

        uint32_t vt[256]; int nvt = tri_extract(tlow, vt);
        nvt = tri_sort_dedup(vt, nvt);

        if (tri_inter(pt, npt, vt, nvt) >= min_ov) {
            int ed = fzy_substring_ed(qlow, ql, tlow, tl, max_k);
            if (ed <= max_k) {
                scored_buf[nscored].doc_id = d;
                scored_buf[nscored].ed = ed;
                nscored++;
                for (uint32_t j = i; j < nc; j++)
                    if (cand[j] == d) seen[j] = 1;
            }
        }
    }
    if (seen_alloc) free(seen);

    qsort(scored_buf, nscored, sizeof(scored_buf[0]), scored_cmp);
    int out = 0;
    for (int i = 0; i < nscored && out < max_results; i++)
        results[out++] = scored_buf[i].doc_id;
    return out;
}

/* ── Substring search (trigram-assisted) ── */
static int ss_search(const char *needle, uint32_t *results, int max_results) {
    if (disk_hdr->tri_ntrigrams == 0) tri_rebuild();

    int nl = strlen(needle);
    if (nl < 3) {
        /* Short needle: linear scan */
        int found = 0;
        for (uint32_t d = 0; d < disk_hdr->num_docs && found < max_results; d++) {
            const char *text = doc_text(d);
            if (text && strcasestr(text, needle))
                results[found++] = d;
        }
        return found;
    }

    char nlow[256];
    int nllen = nl; if (nllen > 255) nllen = 255;
    for (int i = 0; i < nllen; i++) nlow[i] = (char)tolower((uint8_t)needle[i]);
    nlow[nllen] = 0;

    uint32_t pt[256]; int npt = tri_extract(nlow, pt);
    npt = tri_sort_dedup(pt, npt);

    int best_t = -1; uint32_t best_n = 0xFFFFFFFF;
    for (int i = 0; i < npt; i++) {
        uint32_t cnt; tri_bucket(pt[i], &cnt);
        if (cnt > 0 && cnt < best_n) { best_n = cnt; best_t = (int)pt[i]; }
    }
    if (best_t < 0) return 0;

    uint32_t nc, *cand = tri_bucket((uint32_t)best_t, &nc);
    if (!cand || nc > 100000) nc = 100000;

    uint8_t seen_static[4096], *seen;
    int seen_alloc = 0;
    if (nc <= sizeof(seen_static)) {
        seen = seen_static; memset(seen, 0, nc);
    } else { seen = calloc(nc, 1); seen_alloc = 1; }

    int found = 0;
    for (uint32_t i = 0; i < nc && found < max_results; i++) {
        if (seen[i]) continue;
        uint32_t d = cand[i];
        const char *text = doc_text(d);
        if (!text) continue;
        if (strcasestr(text, needle)) {
            results[found++] = d;
            for (uint32_t j = i; j < nc; j++)
                if (cand[j] == d) seen[j] = 1;
        }
    }
    if (seen_alloc) free(seen);
    return found;
}

/* ═══════════════════════════════════════════════════════
 * COMMANDS
 * ═══════════════════════════════════════════════════════ */

static void cmd_add(int argc, char **argv) {
    if (argc < 2) { printf("Usage: add TEXT\n"); return; }
    /* Reassemble text (may contain spaces) */
    char text[4096]; text[0] = 0;
    for (int i = 1; i < argc; i++) {
        if (i > 1) strcat(text, " ");
        strcat(text, argv[i]);
    }
    uint32_t id = doc_add(text);
    if (id == (uint32_t)-1) { printf("Failed\n"); return; }
    disk_hdr->tri_ntrigrams = 0;  /* invalidate trigram index */
    printf("ok doc=%u\n", id);
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

    /* Rebuild trigram index if stale */
    if (disk_hdr->tri_ntrigrams == 0) {
        fprintf(stderr, "Rebuilding index... "); fflush(stderr);
        tri_rebuild();
        fprintf(stderr, "done\n");
    }

    uint32_t results[MAX_RES];
    int n = fzy_search(query, max_k, results, MAX_RES);
    for (int i = 0; i < n; i++) {
        const char *text = doc_text(results[i]);
        int ed = 0;
        if (text) {
            int tl = doc_len(results[i]);
            char tlow[256]; int tllen = tl; if (tllen > 255) tllen = 255;
            for (int j = 0; j < tllen; j++) tlow[j] = (char)tolower((uint8_t)text[j]);
            tlow[tllen] = 0;
            ed = fzy_substring_ed(query, strlen(query), tlow, tllen, max_k);
        }
        printf("ed=%d [%u] ", ed, results[i]);
        if (text) {
            int tl = doc_len(results[i]);
            if (tl > 80) tl = 80;
            printf("%.*s%s\n", tl, text, doc_len(results[i]) > 80 ? "..." : "");
        } else printf("\n");
    }
    printf("%d results\n", n);
}

static void cmd_find(int argc, char **argv) {
    if (argc < 2) { printf("Usage: find SUBSTRING\n"); return; }
    char needle[256]; needle[0] = 0;
    for (int i = 1; i < argc; i++) {
        if (i > 1) strcat(needle, " ");
        strcat(needle, argv[i]);
    }

    if (disk_hdr->tri_ntrigrams == 0) {
        fprintf(stderr, "Rebuilding index... "); fflush(stderr);
        tri_rebuild();
        fprintf(stderr, "done\n");
    }

    uint32_t results[MAX_RES];
    int n = ss_search(needle, results, MAX_RES);
    for (int i = 0; i < n; i++) {
        const char *text = doc_text(results[i]);
        printf("[%u] ", results[i]);
        if (text) {
            int tl = doc_len(results[i]);
            if (tl > 80) tl = 80;
            printf("%.*s%s\n", tl, text, doc_len(results[i]) > 80 ? "..." : "");
        } else printf("\n");
    }
    printf("%d results\n", n);
}

static void cmd_delete(int argc, char **argv) {
    if (argc < 2) { printf("Usage: delete ID\n"); return; }
    uint32_t id = (uint32_t)atoi(argv[1]);
    if (id >= disk_hdr->num_docs) { printf("Invalid ID\n"); return; }
    docs[id].deleted = 1;
    docs[id].len |= 0x80000000;
    /* Write deleted flag to mmap */
    uint32_t stored = docs[id].len;
    memcpy(db_map + docs[id].offset, &stored, 4);
    disk_hdr->tri_ntrigrams = 0;
    printf("deleted %u\n", id);
}

static void cmd_count(void) {
    uint32_t n = 0;
    for (uint32_t i = 0; i < disk_hdr->num_docs; i++)
        if (!docs[i].deleted) n++;
    printf("%u docs (%u total, %u deleted)\n",
        n, disk_hdr->num_docs, disk_hdr->num_docs - n);
}

static void cmd_bench(int argc, char **argv) {
    int N = (argc > 1) ? atoi(argv[1]) : 10000;
    if (N < 100) N = 100;
    if (N > 500000) N = 500000;

    /* Generate docs if needed */
    if (disk_hdr->num_docs < (uint32_t)N) {
        fprintf(stderr, "Generating %d documents... ", N);
        const char *words[] = {"the","quick","brown","fox","jumps","over","lazy","dog",
            "amsterdam","rotterdam","utrecht","database","search","fuzzy","index",
            "hash","query","document","text","engine"};
        int nw = 19;
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

    /* Build index */
    if (disk_hdr->tri_ntrigrams == 0) {
        fprintf(stderr, "Building trigram index... "); fflush(stderr);
        clock_t t = clock();
        tri_rebuild();
        double dt = (double)(clock() - t) / CLOCKS_PER_SEC;
        fprintf(stderr, "done (%.3fs, %u trigrams)\n", dt, disk_hdr->tri_ntrigrams);
    }

    printf("╔════════════════════════════════════════════════════════╗\n");
    printf("║  fuzzydb v%d BENCH  —  %u docs                    ║\n",
        VERSION, disk_hdr->num_docs);
    printf("╠════════════════════════════════════════════════════════╣\n");

    /* Fuzzy search */
    {   const char *q = "amsterdm"; int iters = 200;
        clock_t t = clock(); volatile int s = 0;
        for (int i = 0; i < iters; i++) {
            uint32_t found[MAX_RES];
            s += fzy_search(q, 2, found, MAX_RES);
        }
        double dt = (double)(clock() - t) / CLOCKS_PER_SEC;
        printf("║ Fuzzy search (ed≤2)   %8d ops: %8.3fs → %9.1f µs/op ║\n",
            iters, dt, dt * 1e6 / iters);
    }

    /* Substring search */
    {   const char *q = "dam"; int iters = 1000;
        clock_t t = clock(); volatile int s = 0;
        for (int i = 0; i < iters; i++) {
            uint32_t found[MAX_RES];
            s += ss_search(q, found, MAX_RES);
        }
        double dt = (double)(clock() - t) / CLOCKS_PER_SEC;
        printf("║ Substring (trigram)   %8d ops: %8.3fs → %9.1f µs/op ║\n",
            iters, dt, dt * 1e6 / iters);
    }

    /* Linear substring scan (baseline) */
    {   const char *q = "dam"; int iters = 100;
        clock_t t = clock(); volatile int s = 0;
        for (int i = 0; i < iters; i++) {
            for (uint32_t d = 0; d < disk_hdr->num_docs; d++) {
                const char *text = doc_text(d);
                if (text && strcasestr(text, q)) s++;
            }
        }
        double dt = (double)(clock() - t) / CLOCKS_PER_SEC;
        printf("║ Substring (linear)    %8d ops: %8.3fs → %9.1f µs/op ║\n",
            iters, dt, dt * 1e6 / iters);
    }

    /* Insert */
    {   int iters = 1000;
        clock_t t = clock();
        for (int i = 0; i < iters; i++) {
            char buf[64];
            snprintf(buf, 64, "benchmark document number %d", i);
            doc_add(buf);
        }
        double dt = (double)(clock() - t) / CLOCKS_PER_SEC;
        printf("║ Insert (no index)     %8d ops: %8.3fs → %9.1f µs/op ║\n",
            iters, dt, dt * 1e6 / iters);
    }

    /* Memory */
    printf("╠════════════════════════════════════════════════════════╣\n");
    printf("║  MEMORY: %zu MB mmap, %u docs                       ║\n",
        db_size / (1024*1024), disk_hdr->num_docs);
    printf("╚════════════════════════════════════════════════════════╝\n");
}

/* ── REPL ── */
static void cmd_serve(void) {
    char line[PAGE_SIZE];
    printf("fuzzydb> "); fflush(stdout);
    while (fgets(line, sizeof(line), stdin)) {
        int len = strlen(line);
        while (len > 0 && (line[len-1]=='\n'||line[len-1]=='\r')) line[--len]=0;
        if (len == 0) { printf("fuzzydb> "); fflush(stdout); continue; }

        char *tokens[32]; int nt = 0;
        char *tok = strtok(line, " ");
        while (tok && nt < 32) { tokens[nt++] = tok; tok = strtok(NULL, " "); }
        if (nt == 0) continue;

        if (!strcmp(tokens[0], "exit") || !strcmp(tokens[0], "quit")) break;
        else if (!strcmp(tokens[0], "add"))    cmd_add(nt, tokens);
        else if (!strcmp(tokens[0], "search")) cmd_search(nt, tokens);
        else if (!strcmp(tokens[0], "find"))   cmd_find(nt, tokens);
        else if (!strcmp(tokens[0], "delete")) cmd_delete(nt, tokens);
        else if (!strcmp(tokens[0], "count"))  cmd_count();
        else if (!strcmp(tokens[0], "bench"))  cmd_bench(nt, tokens);
        else printf("? %s\n", tokens[0]);
        fflush(stdout);
        printf("fuzzydb> "); fflush(stdout);
    }
}

/* ── Main ── */
int main(int argc, char **argv) {
    if (argc < 2) {
        printf("fuzzydb v%d — fuzzy full-text search database\n\n", VERSION);
        printf("  add TEXT              Add document\n");
        printf("  search QUERY [K]      Fuzzy search (ed ≤ K, default 2)\n");
        printf("  find SUBSTRING        Substring search\n");
        printf("  delete ID             Remove document\n");
        printf("  count                 Document count\n");
        printf("  bench [N]             Run benchmark\n");
        printf("  serve                 Interactive REPL\n");
        return 0;
    }

    if (db_open(DB_FILE) < 0) { fprintf(stderr, "Cannot open %s\n", DB_FILE); return 1; }

    const char *cmd = argv[1];
    if (!strcmp(cmd, "add"))    cmd_add(argc - 1, argv + 1);
    else if (!strcmp(cmd, "search")) cmd_search(argc - 1, argv + 1);
    else if (!strcmp(cmd, "find"))   cmd_find(argc - 1, argv + 1);
    else if (!strcmp(cmd, "delete")) cmd_delete(argc - 1, argv + 1);
    else if (!strcmp(cmd, "count"))  cmd_count();
    else if (!strcmp(cmd, "bench"))  cmd_bench(argc - 1, argv + 1);
    else if (!strcmp(cmd, "serve"))  cmd_serve();
    else printf("? %s\n", cmd);

    db_close();
    return 0;
}
