/*
 * fastdb.c — World's fastest schema-defined columnar database with fuzzy search.
 *
 * Architecture:
 *   - Metadata: heap-allocated TableMeta array (never invalidated by mmap remap)
 *   - Data: mmap'd column arrays (grows via remap, pointers refreshed each op)
 *   - Disk layout: Page 0=header (4096B), Pages 1..N=metadata, Pages N+1..=column data
 *
 * Indexes per column:
 *   - Hash (PlumHash ARX, O(1) equality)
 *   - Sorted (qsort+bsearch, O(log n) range)
 *   - Trigram fuzzy (PlumHash hashing + Myers bit-parallel ed + Damerau fallback)
 *
 * CRC32-C: HW-accelerated (ARM64 crc32cd / x86 SSE4.2) or software fallback.
 *
 * Prime formula: n = 9r + c + 1, c ∈ {0,1,3,4,6,7}.
 *   Each column c has exactly 1 forbidden row residue per prime p ≠ 3:
 *     r_p(c) = -(c+1)·9⁻¹ mod p
 *
 * Commands:
 *   create TABLE col:TYPE ...        Create table with typed columns
 *   alter TABLE add COL:TYPE         Add column to existing table
 *   alter TABLE drop COL             Remove column
 *   insert TABLE val1 val2 ...       Insert row
 *   select TABLE                     Show all rows
 *   select TABLE where COL = VAL     Equality lookup (hash index)
 *   select TABLE range COL LO HI     Range scan (sorted index)
 *   select TABLE search COL PAT      Substring search (TEXT columns)
 *   select TABLE fuzzy COL PAT [K]   Fuzzy search (trigram+Myers+Damerau)
 *   select TABLE prefix COL PAT      Prefix search (TEXT columns)
 *   delete TABLE where COL = VAL     Soft-delete rows
 *   import TABLE FILE                Bulk import CSV/JSONL
 *   tables                           List all tables
 *   verify TABLE                     Check data integrity
 *   bench [N]                        Run benchmark
 *   serve                            Interactive REPL
 *   komma N                          Show decimal column classification
 *
 * Compile: gcc -O3 -march=armv8-a -o fastdb fastdb.c -I.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#include <ctype.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>

#include "fractal_portable.h"
#define PLUMSHASH_IMPLEMENTATION
#include "plumshash.h"

/* ── Compiler hints (local overrides since we don't include fractal_optimal.h) ── */
#if defined(__GNUC__) || defined(__clang__)
#define LIKELY(x)   __builtin_expect(!!(x), 1)
#define UNLIKELY(x) __builtin_expect(!!(x), 0)
#define ACHT_HOT    __attribute__((hot))
#define ACHT_COLD   __attribute__((cold, noinline))
#define ACHT_INLINE __attribute__((always_inline)) static inline
#define PREFETCH_R(addr) __builtin_prefetch((addr), 0, 3)
#define PREFETCH_W(addr) __builtin_prefetch((addr), 1, 3)
#else
#define LIKELY(x)   (x)
#define UNLIKELY(x) (x)
#define ACHT_HOT
#define ACHT_COLD
#define ACHT_INLINE static inline
#define PREFETCH_R(addr) ((void)(addr))
#define PREFETCH_W(addr) ((void)(addr))
#endif

#define PAGE_SIZE       4096
#define MAGIC           0x46415354  /* "FAST" */
#define VERSION         1
#define MAX_TABLES      32
#define MAX_COLUMNS     64
#define MAX_NAME        48
#define MAX_RES         200
#define DB_FILE         "data.fastdb"

/* Column types */
enum { COL_INT, COL_FLOAT, COL_TEXT, COL_BOOL };

/* ── Column metadata ── */
typedef struct {
    char     name[MAX_NAME];
    uint32_t type;
    uint32_t col_offset;    /* byte offset of column data from file start */
    uint32_t col_alloc;     /* allocated bytes for this column */
    uint32_t rows;          /* rows in this column */
    uint32_t width;         /* bytes per element (1/4/8 for fixed, 0 for TEXT) */
    uint32_t hash_offset;   /* byte offset of hash index (0 = none) */
    uint32_t hash_pages;    /* pages used by hash index */
    uint32_t sort_offset;   /* byte offset of sorted index (0 = none) */
    uint32_t sort_alloc;    /* allocated bytes for sorted index */
    uint32_t sort_count;    /* entries in sorted index */
    uint32_t fzy_offset;    /* byte offset of trigram fuzzy index */
    uint32_t fzy_pages;     /* pages used by fuzzy index */
    uint32_t fzy_ntrigrams; /* total trigram entries */
} ColumnMeta;

/* ── Table metadata (heap-allocated, synced to disk) ── */
typedef struct {
    char        name[MAX_NAME];
    uint32_t    num_columns;
    uint32_t    row_count;
    ColumnMeta  columns[MAX_COLUMNS];
    uint32_t    deleted_offset;  /* page offset of deleted bitmap (mmap) */
    uint32_t    deleted_pages;   /* pages for deleted bitmap */
    uint64_t   *deleted;         /* runtime pointer (NULL if not loaded) */
    uint32_t    deleted_words;   /* size in uint64_t */
} TableMeta;

/* ── On-disk header (page 0) ── */
typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint32_t version;
    uint32_t header_crc;
    uint32_t num_tables;
    uint32_t data_page_start;  /* first free page for column data */
    uint8_t   _pad[PAGE_SIZE - 20];
} DiskHeader;

/* ── Global state ── */
static uint8_t    *db_map = NULL;       /* mmap'd file (data only) */
static size_t      db_size = 0;
static int         db_fd = -1;
static DiskHeader *disk_hdr = NULL;     /* pointer into mmap (page 0) */

static TableMeta  *tables = NULL;       /* heap-allocated metadata */
static uint32_t    num_tables = 0;

/* ── CRC32-C (HW-accelerated via fractal_portable.h) ── */
static uint32_t crc32(const void *d, size_t n) {
    const uint8_t *p = (const uint8_t*)d;
    uint32_t c = 0;
    /* Process 8-byte chunks with HW acceleration */
    while (n >= 8) {
        uint64_t v;
        memcpy(&v, p, 8);
        c = fp_crc32c_u64(v) ^ (c >> 8);
        p += 8; n -= 8;
    }
    /* Remainder byte-by-byte */
    for (size_t i = 0; i < n; i++) {
        c = fp_crc32c_u64((uint64_t)p[i]) ^ (c >> 8);
    }
    return c;
}

/* ── Grow file and remap ── */
static int db_grow(size_t new_size) {
    if (new_size <= db_size) return 0;
    new_size = ((new_size + PAGE_SIZE - 1) / PAGE_SIZE) * PAGE_SIZE;
    if (ftruncate(db_fd, new_size) < 0) return -1;
    if (db_map) munmap(db_map, db_size);
    db_map = mmap(NULL, new_size, PROT_READ | PROT_WRITE, MAP_SHARED, db_fd, 0);
    if (db_map == MAP_FAILED) return -1;
    db_size = new_size;
    disk_hdr = (DiskHeader*)db_map;
    return 0;
}

/* ── Sync metadata to disk pages ── */
static void meta_sync(void) {
    /* Write table metadata to pages 1..N */
    uint32_t meta_bytes = num_tables * sizeof(TableMeta);
    uint32_t meta_pages = (meta_bytes + PAGE_SIZE - 1) / PAGE_SIZE;
    size_t needed = (1 + meta_pages) * PAGE_SIZE;
    if (needed > db_size) db_grow(needed);
    memcpy(db_map + PAGE_SIZE, tables, meta_bytes);
    disk_hdr->num_tables = num_tables;
    disk_hdr->header_crc = crc32(disk_hdr, sizeof(DiskHeader));
    /* MAP_SHARED — kernel flushes asynchronously. msync only on close. */
}

/* ── Open database ── */
static int db_open(const char *path) {
    db_fd = open(path, O_RDWR | O_CREAT, 0644);
    if (db_fd < 0) return -1;
    
    struct stat st; fstat(db_fd, &st);
    if (st.st_size == 0) {
        DiskHeader h; memset(&h, 0, sizeof(h));
        h.magic = MAGIC; h.version = VERSION;
        h.data_page_start = 1 + (MAX_TABLES * sizeof(TableMeta) + PAGE_SIZE - 1) / PAGE_SIZE;
        h.header_crc = crc32(&h, sizeof(h));
        write(db_fd, &h, sizeof(h));
        ftruncate(db_fd, PAGE_SIZE);
    }
    
    db_map = mmap(NULL, (st.st_size ? st.st_size : PAGE_SIZE), PROT_READ | PROT_WRITE, MAP_SHARED, db_fd, 0);
    if (db_map == MAP_FAILED) { close(db_fd); return -1; }
    db_size = st.st_size ? st.st_size : PAGE_SIZE;
#if defined(__linux__)
    // madvise disabled;  /* TLB performance */
#endif
    disk_hdr = (DiskHeader*)db_map;
    
    if (disk_hdr->magic != MAGIC) { munmap(db_map, db_size); close(db_fd); return -1; }
    
    /* Allocate heap metadata */
    tables = calloc(MAX_TABLES, sizeof(TableMeta));
    num_tables = disk_hdr->num_tables;
    
    /* Read metadata from disk if tables exist */
    if (num_tables > 0 && db_size >= (1 + (num_tables * sizeof(TableMeta) + PAGE_SIZE - 1) / PAGE_SIZE) * PAGE_SIZE) {
        memcpy(tables, db_map + PAGE_SIZE, num_tables * sizeof(TableMeta));
        /* Zero out runtime pointers that came from disk */
        for (uint32_t i = 0; i < num_tables; i++) {
            tables[i].deleted = NULL;
        }
    }
    return 0;
}

static void db_close(void) {
    meta_sync();
    if (db_map) { munmap(db_map, db_size); db_map = NULL; }
    if (db_fd >= 0) { close(db_fd); db_fd = -1; }
    free(tables); tables = NULL;
}

/* ── Table lookup ── */
static TableMeta *table_find(const char *name) {
    for (uint32_t i = 0; i < num_tables; i++)
        if (strcmp(tables[i].name, name) == 0) return &tables[i];
    return NULL;
}

static ColumnMeta *column_find(TableMeta *t, const char *name) {
    for (uint32_t i = 0; i < t->num_columns; i++)
        if (strcmp(t->columns[i].name, name) == 0) return &t->columns[i];
    return NULL;
}

/* ── Delete bitmap ── */
static void deleted_load(TableMeta *t) {
    if (t->deleted_pages > 0 && t->deleted_offset > 0) {
        t->deleted = (uint64_t*)(db_map + t->deleted_offset);
        t->deleted_words = (t->deleted_pages * PAGE_SIZE) / 8;
    }
}
static int row_deleted(TableMeta *t, uint32_t row) {
    deleted_load(t);
    if (!t->deleted || row >= t->row_count) return 0;
    uint32_t idx = row >> 6;
    if (idx >= t->deleted_words) return 0;
    return (t->deleted[idx] >> (row & 63)) & 1;
}
static void row_mark_deleted(TableMeta *t, uint32_t row) {
    if (row >= t->row_count) return;
    deleted_load(t);
    uint32_t need = (row >> 6) + 1;
    /* Ensure bitmap pages exist */
    if (t->deleted_pages == 0) {
        t->deleted_offset = t->columns[0].col_offset + 2035 * PAGE_SIZE;
        t->deleted_pages = 1;
        size_t need_bytes = t->deleted_offset + PAGE_SIZE;
        if (need_bytes > db_size) db_grow(need_bytes);
        t->deleted = (uint64_t*)(db_map + t->deleted_offset);
        memset(t->deleted, 0, PAGE_SIZE);
        t->deleted_words = PAGE_SIZE / 8;
    }
    if (need > t->deleted_words) {
        /* Grow bitmap */
        uint32_t new_pages = (need * 8 + PAGE_SIZE - 1) / PAGE_SIZE;
        if (new_pages > t->deleted_pages) {
            t->deleted_pages = new_pages;
            size_t need_bytes = t->deleted_offset + new_pages * PAGE_SIZE;
            if (need_bytes > db_size) db_grow(need_bytes);
            t->deleted = (uint64_t*)(db_map + t->deleted_offset);
            t->deleted_words = new_pages * PAGE_SIZE / 8;
        }
    }
    t->deleted[row >> 6] |= (1ULL << (row & 63));
}

/* ── Type helpers ── */
static const char *type_name(uint32_t t) {
    switch(t) { case COL_INT: return "INT"; case COL_FLOAT: return "FLOAT";
                case COL_TEXT: return "TEXT"; case COL_BOOL: return "BOOL"; }
    return "?";
}
static uint32_t type_parse(const char *s) {
    if (!strcasecmp(s, "INT") || !strcasecmp(s, "INTEGER")) return COL_INT;
    if (!strcasecmp(s, "FLOAT") || !strcasecmp(s, "DOUBLE")) return COL_FLOAT;
    if (!strcasecmp(s, "TEXT") || !strcasecmp(s, "STRING")) return COL_TEXT;
    if (!strcasecmp(s, "BOOL") || !strcasecmp(s, "BOOLEAN")) return COL_BOOL;
    return COL_TEXT;
}
static uint32_t type_width(uint32_t t) {
    switch(t) { case COL_INT: return 4; case COL_FLOAT: return 8;
                case COL_BOOL: return 1; default: return 0; }
}

/* ── Column data access ── */
ACHT_INLINE uint8_t *col_ptr(ColumnMeta *c) {
    return db_map + c->col_offset;
}

ACHT_HOT ACHT_INLINE int int_at(ColumnMeta *c, uint32_t row) {
    if (LIKELY(row < c->rows))
        return ((int*)col_ptr(c))[row];
    return 0;
}
ACHT_HOT ACHT_INLINE double float_at(ColumnMeta *c, uint32_t row) {
    if (LIKELY(row < c->rows))
        return ((double*)col_ptr(c))[row];
    return 0;
}
ACHT_HOT ACHT_INLINE const char *text_at(ColumnMeta *c, uint32_t row) {
    if (LIKELY(row < c->rows && c->width == 0))
        return (const char*)(col_ptr(c) + row * 256);
    return NULL;
}

/* ── Allocate column space ── */
static int col_alloc(ColumnMeta *c, uint32_t needed) {
    if (needed <= c->col_alloc) return 0;
    uint32_t new_alloc = (needed + PAGE_SIZE - 1) / PAGE_SIZE * PAGE_SIZE;
    if (new_alloc > 2035 * PAGE_SIZE) new_alloc = 2035 * PAGE_SIZE;  /* hard cap per column */
    uint32_t new_end = c->col_offset + new_alloc;
    if (new_end > db_size) {
        if (db_grow(new_end) < 0) return -1;
    }
    c->col_alloc = new_alloc;
    return 0;
}

/* ═══════════════════════════════════════════════════════
 * HASH INDEX (per-column, PlumHash, O(1) equality lookup)
 * ═══════════════════════════════════════════════════════ */
#define HASH_SLOTS 251  /* prime, per page */

/* Use PlumHash for hash index — best-in-class avalanche + distribution */
#define HASH_KEY(d, n) plumshash((d), (n), 0x9E3779B9)

/* Allocate hash index at fixed position within column (pages 252-255 of 256) */
static int hash_init(ColumnMeta *c) {
    c->hash_pages = 4;
    c->hash_offset = c->col_offset + 2020 * PAGE_SIZE;
    uint8_t *hp = db_map + c->hash_offset;
    size_t need = c->hash_offset + 4 * PAGE_SIZE;
    if (need > db_size && db_grow(need) < 0) return -1;
    memset(hp, 0, 4 * PAGE_SIZE);
    for (uint32_t pg = 0; pg < 4; pg++) {
        uint32_t *slots = (uint32_t*)(hp + pg * PAGE_SIZE);
        slots[0] = HASH_SLOTS * 4;
    }
    return 0;
}

/* Add entry to hash index: key → row_id */
static void hash_add(ColumnMeta *c, const void *key, uint32_t key_len, uint32_t row) {
    if (c->hash_pages == 0) hash_init(c);
    uint32_t slot = HASH_KEY(key, key_len) % (HASH_SLOTS * c->hash_pages);
    uint32_t pg = slot / HASH_SLOTS;
    uint32_t loc = slot % HASH_SLOTS;
    uint8_t *hp = db_map + c->hash_offset + pg * PAGE_SIZE;
    uint32_t *slots = (uint32_t*)hp;
    uint32_t head = slots[1 + loc];
    
    /* Find space in any hash page — scan all pages for free slot */
    int alloc_pg = -1;
    uint32_t free_off = 0;
    for (int p = 0; p < (int)c->hash_pages; p++) {
        uint8_t *pp = db_map + c->hash_offset + p * PAGE_SIZE;
        uint32_t *sp = (uint32_t*)pp;
        uint32_t fo = sp[0];
        if (fo == 0) fo = HASH_SLOTS * 4;
        if (fo + 8 <= PAGE_SIZE) {
            sp[0] = fo + 8;
            alloc_pg = p;
            free_off = p * PAGE_SIZE + fo;  /* flat offset from hash region start */
            break;
        }
    }
    if (alloc_pg < 0) return;  /* all pages full */
    
    uint8_t *entry = db_map + c->hash_offset + free_off;
    memcpy(entry, &row, 4);
    memcpy(entry + 4, &head, 4);
    slots[1 + loc] = free_off;  /* store flat offset */
}

/* Rebuild hash index from column data. Called on first lookup after bulk inserts. */
static void hash_rebuild(ColumnMeta *c) {
    if (!c || c->rows == 0) return;
    hash_init(c);
    if (c->hash_pages == 0) return;
    for (uint32_t r = 0; r < c->rows; r++) {
        void *key; uint32_t klen;
        switch (c->type) {
        case COL_INT: { int v = ((int*)col_ptr(c))[r]; key = &v; klen = 4; break; }
        case COL_FLOAT: { double v = ((double*)col_ptr(c))[r]; key = &v; klen = 8; break; }
        case COL_BOOL: { uint8_t v = ((uint8_t*)col_ptr(c))[r]; key = &v; klen = 1; break; }
        case COL_TEXT: {
            const char *v = text_at(c, r);
            key = (void*)v; klen = v ? strlen(v) : 3;
            break;
        }
        default: continue;
        }
        hash_add(c, key, klen, r);
    }
}

/* Lookup rows by exact key. Returns count, fills rows[]. */
static int hash_lookup(ColumnMeta *c, const void *key, uint32_t key_len, uint32_t *rows, int max) {
    if (c->hash_pages == 0) hash_rebuild(c);  /* lazy rebuild on first lookup */
    if (c->hash_pages == 0) return -1;
    uint32_t slot = HASH_KEY(key, key_len) % (HASH_SLOTS * c->hash_pages);
    uint32_t pg = slot / HASH_SLOTS;
    uint32_t loc = slot % HASH_SLOTS;
    uint8_t *hp = db_map + c->hash_offset + pg * PAGE_SIZE;
    uint32_t *slots = (uint32_t*)hp;
    uint32_t off = slots[1 + loc];
    int count = 0;
    uint8_t *hash_base = db_map + c->hash_offset;  /* flat base for entire hash region */
    while (off && count < max) {
        uint8_t *entry = hash_base + off;  /* flat offset — works across pages */
        uint32_t r, next;
        memcpy(&r, entry, 4);
        memcpy(&next, entry + 4, 4);
        if (r < c->rows) rows[count++] = r;
        off = next;
    }
    return count;
}

/* ═══════════════════════════════════════════════════════
 * SORTED INDEX (per-column, O(log n) range/equality)
 * ═══════════════════════════════════════════════════════ */
typedef struct { uint32_t row; int32_t ival; double fval; } SortEntry;

static int sort_cmp_int(const void *a, const void *b) {
    return ((SortEntry*)a)->ival - ((SortEntry*)b)->ival;
}
static int sort_cmp_float(const void *a, const void *b) {
    double d = ((SortEntry*)a)->fval - ((SortEntry*)b)->fval;
    return (d > 0) - (d < 0);
}

static void text_idx_build(ColumnMeta *c);

/* Build sorted index for a numeric column — delegates to text_idx_build for TEXT */
static void sort_build(ColumnMeta *c) {
    if (c->type == COL_TEXT) { text_idx_build(c); return; }
    if (c->type != COL_INT && c->type != COL_FLOAT) return;
    uint32_t n = c->rows;
    SortEntry *entries = malloc(n * sizeof(SortEntry));
    for (uint32_t i = 0; i < n; i++) {
        entries[i].row = i;
        if (c->type == COL_INT) entries[i].ival = ((int*)col_ptr(c))[i];
        else entries[i].fval = ((double*)col_ptr(c))[i];
    }
    qsort(entries, n, sizeof(SortEntry), c->type == COL_INT ? sort_cmp_int : sort_cmp_float);
    /* Allocate pages for sorted index */
    c->sort_count = n;
    c->sort_alloc = n * sizeof(SortEntry);
    c->sort_offset = c->col_offset + 1500 * PAGE_SIZE;  /* fixed position */
    size_t need = c->sort_offset + c->sort_alloc;
    if (need > db_size) db_grow(need);
    memcpy(db_map + c->sort_offset, entries, n * sizeof(SortEntry));
    free(entries);
}

/* Binary search for first >= val in sorted index. Returns index, or sort_count if all < val. */
static uint32_t sort_lower_bound(ColumnMeta *c, double val) {
    SortEntry *e = (SortEntry*)(db_map + c->sort_offset);
    uint32_t lo = 0, hi = c->sort_count;
    while (lo < hi) {
        uint32_t m = (lo + hi) / 2;
        double v = (c->type == COL_INT) ? (double)e[m].ival : e[m].fval;
        if (v >= val) hi = m;
        else lo = m + 1;
    }
    return lo;
}

/* ── TEXT sorted index: uint32_t row indices sorted by text_at() content ── */
static ColumnMeta *text_sort_col;  /* context for comparator (single-threaded) */

static int text_sort_cmp(const void *a, const void *b) {
    uint32_t ra = *(const uint32_t*)a, rb = *(const uint32_t*)b;
    const char *ta = text_at(text_sort_col, ra);
    const char *tb = text_at(text_sort_col, rb);
    if (!ta) return tb ? -1 : 0;
    if (!tb) return 1;
    return strcmp(ta, tb);
}

static void text_idx_build(ColumnMeta *c) {
    if (c->type != COL_TEXT || c->rows == 0) return;
    uint32_t n = c->rows;
    uint32_t *idx = malloc(n * sizeof(uint32_t));
    for (uint32_t i = 0; i < n; i++) idx[i] = i;
    text_sort_col = c;
    qsort(idx, n, sizeof(uint32_t), text_sort_cmp);
    text_sort_col = NULL;

    c->sort_count = n;
    c->sort_alloc = n * sizeof(uint32_t);
    c->sort_offset = c->col_offset + 1500 * PAGE_SIZE;
    size_t need = c->sort_offset + c->sort_alloc;
    if (need > db_size) db_grow(need);
    memcpy(db_map + c->sort_offset, idx, n * sizeof(uint32_t));
    free(idx);
}

/* Binary search for first text >= prefix (prefix-only strncmp). */
static uint32_t text_lower_bound(ColumnMeta *c, const char *prefix, int plen) {
    uint32_t *idx = (uint32_t*)(db_map + c->sort_offset);
    uint32_t lo = 0, hi = c->sort_count;
    while (lo < hi) {
        uint32_t m = (lo + hi) / 2;
        const char *v = text_at(c, idx[m]);
        if (!v) { lo = m + 1; continue; }
        if (strncmp(v, prefix, plen) >= 0) hi = m;
        else lo = m + 1;
    }
    return lo;
}

/* Binary search for first text > prefix (upper bound for prefix range). */
static uint32_t text_upper_bound(ColumnMeta *c, const char *prefix, int plen) {
    uint32_t *idx = (uint32_t*)(db_map + c->sort_offset);
    uint32_t lo = 0, hi = c->sort_count;
    while (lo < hi) {
        uint32_t m = (lo + hi) / 2;
        const char *v = text_at(c, idx[m]);
        if (!v) { lo = m + 1; continue; }
        if (strncmp(v, prefix, plen) > 0) hi = m;
        else lo = m + 1;
    }
    return lo;
}

/* ═══════════════════════════════════════════════════════
 * TRIGRAM FUZZY INDEX (per TEXT column, pages 52-55 of 64)
 * ═══════════════════════════════════════════════════════ */
#define N_TRIGRAM_BUCKETS 1000
#define QLEN 3
#define FZY_MAX_SCORED 512
#define FZY_MYERS_CUTOFF 62  /* max safe 64-bit Myers (63 bits + overflow guard) */

/* Result entry for fuzzy search scoring */
struct fzy_scored { uint32_t row; int ed; };

/* ── Faster trigram hash using PlumHash ── */
ACHT_INLINE int tri_hash(const uint8_t *s) {
    return (int)(plumshash(s, 3, 0x9E3779B9) % N_TRIGRAM_BUCKETS);
}

/* Extract q-grams from string into bucket hashes (unsorted, dedup removed later) */
static int tri_extract(const char *s, int *out) {
    int n = 0, len = strlen(s);
    const uint8_t *u = (const uint8_t*)s;
    for (int i = 0; i <= len - QLEN; i++)
        out[n++] = tri_hash(u + i);
    return n;
}

/* Sort trigrams for intersection; also dedup */
static int tri_sort_dedup(int *a, int n) {
    if (n <= 1) return n;
    /* Insertion sort for small n (typical: 3-60 trigrams) */
    for (int i = 1; i < n; i++) {
        int x = a[i], j = i - 1;
        while (j >= 0 && a[j] > x) { a[j+1] = a[j]; j--; }
        a[j+1] = x;
    }
    /* Dedup in-place */
    int out = 1;
    for (int i = 1; i < n; i++)
        if (a[i] != a[out-1]) a[out++] = a[i];
    return out;
}

/* ── Myers bit-parallel edit distance (strings ≤ 64 chars) ──
 * ~5-10x faster than Wagner-Fischer for short strings.
 * Returns edit distance, or max_k+1 if exceeded. */
ACHT_HOT static int fzy_myers(const char *a, int la, const char *b, int lb, int max_k) {
    if (la > FZY_MYERS_CUTOFF || lb > FZY_MYERS_CUTOFF) return -1;  /* fallback */
    /* Ensure a is the shorter string */
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
        /* score can decrease by at most 1 per remaining character.
         * Only bail if even perfect convergence can't reach ≤ max_k. */
        int remaining = lb - j - 1;
        if (score - remaining > max_k) return max_k + 1;
    }
    return score;
}

/* ── Damerau-Levenshtein with early termination (fallback for long strings) ── */
ACHT_COLD static int fzy_damerau(const char *a, int la, const char *b, int lb, int max_k) {
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

/* Unified edit distance: Myers for speed, Damerau fallback for transpositions */
ACHT_HOT ACHT_INLINE int fzy_edit(const char *a, int la, const char *b, int lb, int max_k) {
    int r = fzy_myers(a, la, b, lb, max_k);
    if (r >= 0 && r <= max_k) return r;   /* Myers passed — done */
    if (r == -1) return fzy_damerau(a, la, b, lb, max_k);  /* too long for Myers */
    /* Myers exceeded max_k — try Damerau for transposition benefit */
    return fzy_damerau(a, la, b, lb, max_k);
}

/* Build trigram fuzzy index for a TEXT column */
static int fzy_build(ColumnMeta *c) {
    if (c->type != COL_TEXT || c->rows == 0) return 0;
    c->fzy_offset = c->col_offset + 2030 * PAGE_SIZE;
    c->fzy_pages = 4;
    size_t need = c->fzy_offset + 4 * PAGE_SIZE;
    if (need > db_size) db_grow(need);
    
    uint8_t *fp = db_map + c->fzy_offset;
    memset(fp, 0, 4 * PAGE_SIZE);
    
    /* Pass 1: count trigrams */
    int *counts = calloc(N_TRIGRAM_BUCKETS, sizeof(int));
    int total = 0;
    for (uint32_t r = 0; r < c->rows; r++) {
        const char *val = text_at(c, r);
        if (!val) continue;
        int tri[256], nt = tri_extract(val, tri);
        for (int j = 0; j < nt; j++) { counts[tri[j]]++; total++; }
    }
    c->fzy_ntrigrams = total;
    
    /* Allocate per-bucket arrays and fill directory */
    uint32_t *dir = (uint32_t*)fp;  /* 1000 uint32_t offsets */
    uint32_t data_off = 1000 * 4;
    for (int t = 0; t < N_TRIGRAM_BUCKETS; t++) {
        dir[t] = data_off;
        data_off += counts[t] * 4;
    }
    if (data_off > 4 * PAGE_SIZE) return -1;  /* too many trigrams */
    
    /* Pass 2: fill buckets */
    int *filled = calloc(N_TRIGRAM_BUCKETS, sizeof(int));
    for (uint32_t r = 0; r < c->rows; r++) {
        const char *val = text_at(c, r);
        if (!val) continue;
        int tri[256], nt = tri_extract(val, tri);
        for (int j = 0; j < nt; j++) {
            int t = tri[j];
            if (filled[t] < counts[t]) {
                int *bucket = (int*)(fp + dir[t]);
                bucket[filled[t]++] = r;
            }
        }
    }
    free(counts); free(filled);
    return 0;
}

/* Get bucket for trigram hash t */
static int *fzy_bucket(ColumnMeta *c, int t, int *count) {
    if (!c || c->fzy_pages == 0) { *count = 0; return NULL; }
    uint8_t *fp = db_map + c->fzy_offset;
    uint32_t *dir = (uint32_t*)fp;
    if (t < 0 || t >= N_TRIGRAM_BUCKETS) { *count = 0; return NULL; }
    /* Count = (next_offset - this_offset) / 4 */
    uint32_t off = dir[t];
    uint32_t next = (t + 1 < N_TRIGRAM_BUCKETS) ? dir[t+1] : 4 * PAGE_SIZE;
    *count = (next - off) / 4;
    return (int*)(fp + off);
}

/* Two-pointer merge intersection of sorted arrays. Returns match count.
 * Both a[] and b[] must be sorted+deduped. O(na + nb). */
static int tri_inter(const int *a, int na, const int *b, int nb) {
    int c = 0, i = 0, j = 0;
    while (i < na && j < nb) {
        if (a[i] < b[j]) i++;
        else if (b[j] < a[i]) j++;
        else { c++; i++; j++; }
    }
    return c;
}

/* qsort comparator for fzy_scored entries */
static int fzy_scored_cmp(const void *va, const void *vb) {
    int ea = ((const struct fzy_scored*)va)->ed;
    int eb = ((const struct fzy_scored*)vb)->ed;
    return (ea > eb) - (ea < eb);
}

/* Fuzzy search on a TEXT column. Results in rows[], sorted by ed. Returns count. */
static int fzy_search(ColumnMeta *c, const char *query, int max_k, uint32_t *rows, int max_rows) {
    if (!c || c->fzy_pages == 0) {
        if (c) fzy_build(c);
        if (!c || c->fzy_pages == 0) return 0;
    }

    /* Lowercase query once */
    char qlow[256];
    int ql = strlen(query); if (ql > 255) ql = 255;
    for (int i = 0; i < ql; i++) qlow[i] = (char)tolower((uint8_t)query[i]);
    qlow[ql] = 0;

    /* Extract + sort + dedup query trigrams */
    int pt[256], npt = tri_extract(qlow, pt);
    if (npt == 0) return 0;
    npt = tri_sort_dedup(pt, npt);

    int min_ov = npt - 3 * max_k; if (min_ov < 1) min_ov = 1;

    struct fzy_scored scored_buf[FZY_MAX_SCORED];
    int nscored = 0;

    /* For very short queries (≤3 trigrams), linear scan with ed */
    if (npt <= 3) {
        for (uint32_t r = 0; r < c->rows && nscored < FZY_MAX_SCORED; r++) {
            const char *val = text_at(c, r);
            if (!val) continue;
            int vl = strlen(val); if (vl > 255) vl = 255;
            if (vl < ql - max_k || vl > ql + max_k) continue;
            int ed = fzy_edit(qlow, ql, val, vl, max_k);
            if (ed <= max_k) {
                scored_buf[nscored].row = r;
                scored_buf[nscored].ed = ed;
                nscored++;
            }
        }
        qsort(scored_buf, nscored, sizeof(scored_buf[0]), fzy_scored_cmp);
        int out = 0;
        for (int i = 0; i < nscored && out < max_rows; i++)
            rows[out++] = scored_buf[i].row;
        return out;
    }

    /* Find smallest bucket as candidate list */
    int best_t = -1, best_n = 0x7FFFFFFF;
    for (int i = 0; i < npt; i++) {
        int cnt; fzy_bucket(c, pt[i], &cnt);
        if (cnt > 0 && cnt < best_n) { best_n = cnt; best_t = pt[i]; }
    }
    if (best_t < 0) return 0;

    int nc, *cand = fzy_bucket(c, best_t, &nc);
    if (!cand) return 0;
    if (nc > 100000) nc = 100000;  /* safety cap */

    /* seen[] on stack: track which candidate rows we've already scored */
    uint8_t seen_static[4096];  /* covers up to 32768 candidates */
    uint8_t *seen;
    int seen_alloc = 0;
    if (nc <= (int)sizeof(seen_static)) {
        seen = seen_static;
        memset(seen, 0, (size_t)nc);
    } else {
        seen = calloc((size_t)nc, 1);
        seen_alloc = 1;
    }

    for (int i = 0; i < nc && nscored < FZY_MAX_SCORED; i++) {
        if (seen[i]) continue;
        int r = cand[i];
        const char *val = text_at(c, r);
        if (!val) continue;

        /* Lowercase value + early length filter */
        char vlow[256];
        int vl = strlen(val); if (vl > 255) vl = 255;
        if (vl < ql - max_k || vl > ql + max_k) continue;  /* length diff > max_k */
        for (int j = 0; j < vl; j++) vlow[j] = (char)tolower((uint8_t)val[j]);
        vlow[vl] = 0;

        int vt[256], nvt = tri_extract(vlow, vt);
        nvt = tri_sort_dedup(vt, nvt);

        if (tri_inter(pt, npt, vt, nvt) >= min_ov) {
            int ed = fzy_edit(qlow, ql, vlow, vl, max_k);
            if (ed <= max_k) {
                scored_buf[nscored].row = (uint32_t)r;
                scored_buf[nscored].ed = ed;
                nscored++;
                /* Mark all occurrences of this row ID as seen */
                for (int j = i; j < nc; j++)
                    if (cand[j] == r) seen[j] = 1;
            }
        }
    }
    if (seen_alloc) free(seen);

    qsort(scored_buf, nscored, sizeof(scored_buf[0]), fzy_scored_cmp);

    int out = 0;
    for (int i = 0; i < nscored && out < max_rows; i++)
        rows[out++] = scored_buf[i].row;
    return out;
}

/* ── Substring search via trigram index ──
 * Uses the trigram buckets to find candidate rows, then verifies
 * with strcasestr. Falls back to linear scan for needles < 3 chars
 * or when the trigram index isn't built. */
static int ss_search(ColumnMeta *c, const char *needle, uint32_t *rows, int max_rows) {
    if (!c) return 0;

    /* Build trigram index if missing */
    if (c->fzy_pages == 0) fzy_build(c);

    int nl = strlen(needle);
    if (nl < 3 || c->fzy_pages == 0) {
        /* Short needle or no index: linear scan */
        int found = 0;
        for (uint32_t r = 0; r < c->rows && found < max_rows; r++) {
            const char *v = text_at(c, r);
            if (v && strcasestr(v, needle)) rows[found++] = r;
        }
        return found;
    }

    /* Lowercase needle for trigram extraction */
    char nlow[256];
    int nllen = nl; if (nllen > 255) nllen = 255;
    for (int i = 0; i < nllen; i++) nlow[i] = (char)tolower((uint8_t)needle[i]);
    nlow[nllen] = 0;

    /* Extract and dedup trigrams */
    int pt[256], npt = tri_extract(nlow, pt);
    npt = tri_sort_dedup(pt, npt);

    /* Find smallest bucket as candidate list */
    int best_t = -1, best_n = 0x7FFFFFFF;
    for (int i = 0; i < npt; i++) {
        int cnt; fzy_bucket(c, pt[i], &cnt);
        if (cnt > 0 && cnt < best_n) { best_n = cnt; best_t = pt[i]; }
    }
    if (best_t < 0) return 0;

    int nc, *cand = fzy_bucket(c, best_t, &nc);
    if (!cand || nc <= 0) return 0;
    if (nc > 100000) nc = 100000;

    /* seen[] on stack to avoid re-checking duplicates */
    uint8_t seen_static[4096];
    uint8_t *seen;
    int seen_alloc = 0;
    if (nc <= (int)sizeof(seen_static)) {
        seen = seen_static;
        memset(seen, 0, (size_t)nc);
    } else {
        seen = calloc((size_t)nc, 1);
        seen_alloc = 1;
    }

    int found = 0;
    for (int i = 0; i < nc && found < max_rows; i++) {
        if (seen[i]) continue;
        int r = cand[i];
        const char *val = text_at(c, r);
        if (!val) continue;
        if (strcasestr(val, needle)) {
            rows[found++] = (uint32_t)r;
            for (int j = i; j < nc; j++)
                if (cand[j] == r) seen[j] = 1;
        }
    }
    if (seen_alloc) free(seen);
    return found;
}

/* ═══════════════════════════════════════════════════════
 * COMMANDS
 * ═══════════════════════════════════════════════════════ */

/* ── CREATE TABLE ── */
static int cmd_create(int argc, char **argv) {
    if (argc < 3) { printf("Usage: create TABLE col:TYPE ...\n"); return -1; }
    if (num_tables >= MAX_TABLES) { printf("Too many tables\n"); return -1; }
    if (table_find(argv[1])) { printf("Table '%s' already exists\n", argv[1]); return -1; }
    
    TableMeta *t = &tables[num_tables];
    memset(t, 0, sizeof(TableMeta));
    strncpy(t->name, argv[1], MAX_NAME - 1);
    
    uint32_t next_offset = disk_hdr->data_page_start * PAGE_SIZE;
    for (int i = 2; i < argc && t->num_columns < MAX_COLUMNS; i++) {
        char *colon = strchr(argv[i], ':');
        if (!colon) continue;
        *colon = 0;
        ColumnMeta *c = &t->columns[t->num_columns];
        strncpy(c->name, argv[i], MAX_NAME - 1);
        c->type = type_parse(colon + 1);
        c->width = type_width(c->type);
        c->col_offset = next_offset;
        c->col_alloc = 0;
        c->rows = 0;
        next_offset += 2048 * PAGE_SIZE;  /* 8MB per column */
        t->num_columns++;
    }
    
    /* Ensure space for column data */
    size_t needed = next_offset;
    if (needed > db_size) db_grow(needed);
    disk_hdr->data_page_start = next_offset / PAGE_SIZE;
    
    num_tables++;
    meta_sync();
    
    printf("Created '%s' (%u cols)\n", t->name, t->num_columns);
    for (uint32_t i = 0; i < t->num_columns; i++)
        printf("  %s: %s\n", t->columns[i].name, type_name(t->columns[i].type));
    return 0;
}

/* ── ALTER TABLE ── */
static int cmd_alter(int argc, char **argv) {
    if (argc < 4) { printf("Usage: alter TABLE add COL:TYPE | drop COL\n"); return -1; }
    TableMeta *t = table_find(argv[1]);
    if (!t) { printf("Table '%s' not found\n", argv[1]); return -1; }
    
    if (!strcmp(argv[2], "add") && argc >= 4) {
        if (t->num_columns >= MAX_COLUMNS) { printf("Too many columns\n"); return -1; }
        char *colon = strchr(argv[3], ':');
        if (!colon) { printf("Expected col:TYPE\n"); return -1; }
        *colon = 0;
        ColumnMeta *c = &t->columns[t->num_columns];
        memset(c, 0, sizeof(ColumnMeta));
        strncpy(c->name, argv[3], MAX_NAME - 1);
        c->type = type_parse(colon + 1);
        c->width = type_width(c->type);
        c->col_offset = disk_hdr->data_page_start * PAGE_SIZE;
        c->col_alloc = PAGE_SIZE;
        c->rows = 0;
        /* Backfill NULLs for existing rows */
        if (t->row_count > 0) {
            col_alloc(c, (c->width ? c->width : 256) * t->row_count + PAGE_SIZE);
            c->rows = t->row_count;
            if (c->width == 0) { /* TEXT — init 256-byte slots to empty */
                char *slots = (char*)col_ptr(c);
                memset(slots, 0, t->row_count * 256);
            }
        }
        size_t needed = c->col_offset + c->col_alloc;
        if (needed > db_size) db_grow(needed);
        disk_hdr->data_page_start = (c->col_offset + c->col_alloc + PAGE_SIZE - 1) / PAGE_SIZE;
        t->num_columns++;
        meta_sync();
        printf("Added column '%s' (%s) to '%s'\n", c->name, type_name(c->type), t->name);
    }
    else if (!strcmp(argv[2], "drop") && argc >= 4) {
        ColumnMeta *c = column_find(t, argv[3]);
        if (!c) { printf("Column '%s' not found\n", argv[3]); return -1; }
        /* Mark as dropped by clearing name */
        c->name[0] = 0;
        meta_sync();
        printf("Dropped column '%s' from '%s'\n", argv[3], t->name);
    }
    else { printf("Usage: alter TABLE add COL:TYPE | drop COL\n"); return -1; }
    return 0;
}

/* ── IMPORT CSV/JSONL ── */
static int cmd_insert(int argc, char **argv);  /* forward */
static int cmd_import(int argc, char **argv) {
    if (argc < 3) { printf("Usage: import TABLE FILE\n"); return -1; }
    TableMeta *t = table_find(argv[1]);
    if (!t) { printf("Table '%s' not found\n", argv[1]); return -1; }
    FILE *f = fopen(argv[2], "r");
    if (!f) { printf("Cannot open '%s'\n", argv[2]); return -1; }
    
    char line[65536];
    int loaded = 0, is_csv = strstr(argv[2], ".csv") != NULL;
    
    /* For CSV: first line is header */
    if (is_csv) fgets(line, sizeof(line), f);  /* skip header */
    
    while (fgets(line, sizeof(line), f)) {
        int len = strlen(line);
        while (len > 0 && (line[len-1]=='\n'||line[len-1]=='\r')) line[--len]=0;
        if (len < 2) continue;
        
        /* Build argv for insert */
        char *vals[32]; int nv = 0;
        vals[nv++] = "import"; vals[nv++] = argv[1];  /* dummy, matches insert signature */
        
        if (is_csv) {
            char *tok = strtok(line, ",");
            while (tok && nv < 32) { vals[nv++] = tok; tok = strtok(NULL, ","); }
        } else {
            /* JSONL: crude parse — extract values between quotes */
            char *p = line;
            while (*p && nv < 32) {
                while (*p && *p != '"') p++;
                if (!*p) break;
                p++; char *start = p;
                while (*p && *p != '"') {
                    if (*p == '\\' && *(p+1)) p++;
                    p++;
                }
                *p = 0; p++;
                vals[nv++] = start;
            }
        }
        if (nv >= 3) cmd_insert(nv, vals);
        loaded++;
        if (loaded % 500 == 0) { meta_sync(); fprintf(stderr, "  %d rows...\n", loaded); }
    }
    fclose(f);
    meta_sync();
    printf("Imported %d rows into '%s'\n", loaded, t->name);
    return 0;
}

/* ── DELETE ── */
static int cmd_delete(int argc, char **argv) {
    /* delete TABLE where COL = VAL */
    if (argc < 5 || strcmp(argv[2], "where") != 0) {
        printf("Usage: delete TABLE where COL = VAL\n"); return -1;
    }
    TableMeta *t = table_find(argv[1]);
    if (!t) { printf("Table '%s' not found\n", argv[1]); return -1; }
    
    char *col = argv[3], *val = argv[5];  /* argv[4] is "=" */
    ColumnMeta *c = column_find(t, col);
    if (!c) { printf("Column '%s' not found\n", col); return -1; }
    
    int deleted = 0;
    for (uint32_t r = 0; r < t->row_count; r++) {
        if (row_deleted(t, r)) continue;
        int match = 0;
        if (c->type == COL_INT) match = (int_at(c, r) == atoi(val));
        else if (c->type == COL_TEXT) {
            const char *v = text_at(c, r);
            match = v && !strcmp(v, val);
        } else if (c->type == COL_FLOAT) match = (float_at(c, r) == atof(val));
        if (match) { row_mark_deleted(t, r); deleted++; }
    }
    printf("Deleted %d rows from '%s'\n", deleted, t->name);
    return 0;
}

/* ── INSERT ── */
static int cmd_insert(int argc, char **argv) {
    if (argc < 2) { printf("Usage: insert TABLE val1 val2 ...\n"); return -1; }
    TableMeta *t = table_find(argv[1]);
    if (!t) { printf("Table '%s' not found\n", argv[1]); return -1; }
    
    int nvals = 0;
    for (int i = 2; i < argc; i++) nvals++;
    if (nvals != (int)t->num_columns) {
        printf("Expected %u values, got %d\n", t->num_columns, nvals);
        return -1;
    }
    
    uint32_t row = t->row_count;
    
    for (uint32_t ci = 0; ci < t->num_columns; ci++) {
        if (t->columns[ci].name[0] == 0) continue;  /* dropped column */
        ColumnMeta *c = &t->columns[ci];
        const char *val = argv[2 + ci];
        
        if (c->width > 0) {
            /* Fixed-width column */
            uint32_t need = (row + 1) * c->width;
            col_alloc(c, need);
            uint8_t *p = col_ptr(c);
            switch (c->type) {
            case COL_INT:  ((int*)p)[row] = atoi(val); break;
            case COL_FLOAT: ((double*)p)[row] = atof(val); break;
            case COL_BOOL: p[row] = (val[0]=='1'||val[0]=='t'||val[0]=='T')?1:0; break;
            }
        } else {
            /* TEXT column: fixed 256-byte slots */
            uint32_t vlen = strlen(val);
            uint32_t need = (row + 1) * 256;
            col_alloc(c, need);
            char *slot = (char*)(col_ptr(c) + row * 256);
            uint32_t copy = vlen < 255 ? vlen : 255;
            memcpy(slot, val, copy);
            slot[copy] = 0;
        }
        c->rows = row + 1;
        /* Hash index built lazily on first lookup — O(N) rebuild, O(1) thereafter */
    }
    
    t->row_count++;
    /* meta_sync deferred — caller should sync after batch */
    return 0;
}

/* ── SELECT ── */
static void print_row(TableMeta *t, uint32_t row) {
    if (row_deleted(t, row)) return;
    printf("[%u] ", row);
    for (uint32_t ci = 0; ci < t->num_columns; ci++) {
        ColumnMeta *c = &t->columns[ci];
        if (c->name[0] == 0) continue;
        if (row >= c->rows) { printf("%s=NULL ", c->name); continue; }
        switch (c->type) {
        case COL_INT:  printf("%s=%d ", c->name, int_at(c, row)); break;
        case COL_FLOAT: printf("%s=%.2f ", c->name, float_at(c, row)); break;
        case COL_TEXT: {
            const char *s = text_at(c, row);
            printf("%s=%s ", c->name, s ? s : "NULL");
            break;
        }
        case COL_BOOL: printf("%s=%d ", c->name, col_ptr(c)[row]); break;
        }
    }
    printf("\n");
}

static int cmd_select(int argc, char **argv) {
    if (argc < 2) { printf("Usage: select TABLE [where COL = VAL] [range COL LO HI] [search COL PAT]\n"); return -1; }
    TableMeta *t = table_find(argv[1]);
    if (!t) { printf("Table '%s' not found\n", argv[1]); return -1; }
    
    char *where_col=NULL, *where_val=NULL;
    char *range_col=NULL, *range_lo=NULL, *range_hi=NULL;
    char *search_col=NULL, *search_pat=NULL;
    char *fuzzy_col=NULL, *fuzzy_pat=NULL;
    int   fuzzy_k = 1;
    char *prefix_col=NULL, *prefix_pat=NULL;
    
    for (int i = 2; i < argc; i++) {
        if (!strcmp(argv[i], "where") && i+3 < argc) { where_col=argv[++i]; (void)argv[++i]; where_val=argv[++i]; }
        else if (!strcmp(argv[i], "range") && i+3 < argc) { range_col=argv[++i]; range_lo=argv[++i]; range_hi=argv[++i]; }
        else if (!strcmp(argv[i], "search") && i+2 < argc) { search_col=argv[++i]; search_pat=argv[++i]; }
        else if (!strcmp(argv[i], "fuzzy") && i+2 < argc) { fuzzy_col=argv[++i]; fuzzy_pat=argv[++i]; if (i+1<argc && isdigit(argv[i+1][0])) fuzzy_k=atoi(argv[++i]); }
        else if (!strcmp(argv[i], "prefix") && i+2 < argc) { prefix_col=argv[++i]; prefix_pat=argv[++i]; }
    }
    
    uint32_t found = 0;
    
    /* Use hash index for equality lookup (O(1)) */
    if (where_col && where_val && !range_col && !search_col) {
        ColumnMeta *c = column_find(t, where_col);
        if (c) {
            uint32_t rows[MAX_RES];
            int nc = 0;
            switch (c->type) {
            case COL_INT: { int v = atoi(where_val); nc = hash_lookup(c, &v, 4, rows, MAX_RES); break; }
            case COL_TEXT: nc = hash_lookup(c, where_val, strlen(where_val), rows, MAX_RES); break;
            case COL_FLOAT: { double v = atof(where_val); nc = hash_lookup(c, &v, 8, rows, MAX_RES); break; }
            case COL_BOOL: { uint8_t v = (where_val[0]=='1'||where_val[0]=='t'||where_val[0]=='T'); nc = hash_lookup(c, &v, 1, rows, MAX_RES); break; }
            }
            for (int i = 0; i < nc && found < MAX_RES; i++) {
                print_row(t, rows[i]); found++;
            }
        }
    }
    
    /* Use sorted index for range query (O(log n + k)) */
    if (range_col && range_lo && range_hi && !search_col) {
        ColumnMeta *c = column_find(t, range_col);
        if (c && (c->type == COL_INT || c->type == COL_FLOAT)) {
            if (c->sort_count != c->rows) sort_build(c);  /* rebuild if stale */
            if (c->sort_count > 0) {
                double lo = atof(range_lo), hi = atof(range_hi);
                uint32_t si = sort_lower_bound(c, lo);
                SortEntry *e = (SortEntry*)(db_map + c->sort_offset);
                for (; si < c->sort_count && found < MAX_RES; si++) {
                    double v = (c->type == COL_INT) ? (double)e[si].ival : e[si].fval;
                    if (v > hi) break;
                    if (where_col && where_val) {
                        /* Combined where + range: check the where condition on this row */
                        ColumnMeta *wc = column_find(t, where_col);
                        if (wc) {
                            if (wc->type == COL_INT && int_at(wc, e[si].row) != atoi(where_val)) continue;
                            if (wc->type == COL_TEXT) {
                                const char *tv = text_at(wc, e[si].row);
                                if (!tv || strcmp(tv, where_val)) continue;
                            }
                        }
                    }
                    print_row(t, e[si].row); found++;
                }
            }
        }
    }
    
    /* Substring search via trigram index */
    if (search_col && search_pat) {
        ColumnMeta *c = column_find(t, search_col);
        if (c && c->type == COL_TEXT) {
            uint32_t rows[MAX_RES];
            int n = ss_search(c, search_pat, rows, MAX_RES);
            for (int i = 0; i < n && found < MAX_RES; i++)
                { print_row(t, rows[i]); found++; }
        }
    }
    
    /* Fuzzy search with trigram index + Damerau-Levenshtein */
    if (fuzzy_col && fuzzy_pat) {
        ColumnMeta *c = column_find(t, fuzzy_col);
        if (c && c->type == COL_TEXT) {
            uint32_t rows[MAX_RES];
            /* Get results */
            int n = fzy_search(c, fuzzy_pat, fuzzy_k, rows, MAX_RES);
            for (int i = 0; i < n; i++) {
                /* Re-compute ed for display */
                const char *val = text_at(c, rows[i]);
                int ed = val ? fzy_edit(fuzzy_pat, strlen(fuzzy_pat), val, strlen(val), fuzzy_k) : fuzzy_k;
                printf("ed=%d ", ed);
                print_row(t, rows[i]);
            }
            found = n;
        }
    }
    
    /* Prefix search via TEXT sorted index (binary search) */
    if (prefix_col && prefix_pat) {
        ColumnMeta *c = column_find(t, prefix_col);
        if (c && c->type == COL_TEXT) {
            if (c->sort_count != c->rows) sort_build(c);
            if (c->sort_count > 0) {
                int plen = strlen(prefix_pat);
                uint32_t lo = text_lower_bound(c, prefix_pat, plen);
                uint32_t hi = text_upper_bound(c, prefix_pat, plen);
                uint32_t *idx = (uint32_t*)(db_map + c->sort_offset);
                for (uint32_t i = lo; i < hi && found < MAX_RES; i++)
                    { print_row(t, idx[i]); found++; }
            }
        }
    }
    
    /* Full scan fallback */
    if (found == 0 && !where_col && !range_col && !search_col && !fuzzy_col) {
        for (uint32_t r = 0; r < t->row_count && r < 50; r++)
            print_row(t, r);
        found = t->row_count;
    }
    /* Linear scan fallback for where without hash index */
    if (found == 0 && where_col && where_val && !range_col && !search_col) {
        ColumnMeta *c = column_find(t, where_col);
        for (uint32_t r = 0; r < t->row_count && found < MAX_RES; r++) {
            int match = 0;
            if (c->type == COL_INT) match = (int_at(c, r) == atoi(where_val));
            else if (c->type == COL_TEXT) {
                const char *v = text_at(c, r);
                match = v && !strcmp(v, where_val);
            } else if (c->type == COL_FLOAT) match = (float_at(c, r) == atof(where_val));
            if (match) { print_row(t, r); found++; }
        }
    }
    printf("%u rows\n", found > 50 ? t->row_count : found);
    return 0;
}

/* ── TABLES ── */
static int cmd_tables(void) {
    for (uint32_t i = 0; i < num_tables; i++) {
        TableMeta *t = &tables[i];
        int active_cols = 0;
        for (uint32_t j = 0; j < t->num_columns; j++)
            if (t->columns[j].name[0]) active_cols++;
        printf("%s (%d cols, %u rows)\n", t->name, active_cols, t->row_count);
        for (uint32_t j = 0; j < t->num_columns; j++)
            if (t->columns[j].name[0])
                printf("  %s: %s\n", t->columns[j].name, type_name(t->columns[j].type));
    }
    if (num_tables == 0) printf("No tables\n");
    return 0;
}

/* ── VERIFY — check data integrity ── */
static int cmd_verify(int argc, char **argv) {
    if (argc < 2) { printf("Usage: verify TABLE\n"); return -1; }
    TableMeta *t = table_find(argv[1]);
    if (!t) { printf("Table '%s' not found\n", argv[1]); return -1; }
    
    int issues = 0;
    printf("Verifying '%s'...\n", t->name);
    
    /* Check column row counts match */
    for (uint32_t ci = 0; ci < t->num_columns; ci++) {
        ColumnMeta *c = &t->columns[ci];
        if (c->name[0] == 0) continue;
        if (c->rows != t->row_count) {
            printf("  COL MISMATCH: %s.rows=%u, table.rows=%u\n", c->name, c->rows, t->row_count);
            issues++;
        }
    }
    
    /* Check sorted index matches for numeric columns */
    for (uint32_t ci = 0; ci < t->num_columns; ci++) {
        ColumnMeta *c = &t->columns[ci];
        if (c->name[0] == 0) continue;
        if (c->type == COL_INT || c->type == COL_FLOAT) {
            if (c->sort_count > 0 && c->sort_count != c->rows) {
                printf("  STALE SORT: %s sort_count=%u != rows=%u\n", c->name, c->sort_count, c->rows);
                issues++;
            }
        }
    }
    
    /* Check hash index has valid entries */
    for (uint32_t ci = 0; ci < t->num_columns; ci++) {
        ColumnMeta *c = &t->columns[ci];
        if (c->name[0] == 0) continue;
        if (c->hash_pages > 0) {
            uint32_t total_entries = 0;
            for (uint32_t pg = 0; pg < c->hash_pages; pg++) {
                uint32_t *slots = (uint32_t*)(db_map + c->hash_offset + pg * PAGE_SIZE);
                if (slots[0] < HASH_SLOTS * 4 || slots[0] > PAGE_SIZE) {
                    printf("  HASH CORRUPT: %s page %u free ptr=%u\n", c->name, pg, slots[0]);
                    issues++;
                }
                for (int s = 1; s <= HASH_SLOTS; s++) {
                    uint32_t off = slots[s];
                    while (off) {
                        uint8_t *entry = db_map + c->hash_offset + off;
                        uint32_t r, next;
                        memcpy(&r, entry, 4);
                        memcpy(&next, entry + 4, 4);
                        if (r >= c->rows) {
                            printf("  HASH BAD ROW: %s slot %d row=%u >= %u\n", c->name, s, r, c->rows);
                            issues++;
                        }
                        total_entries++;
                        off = next;
                        if (total_entries > 1000000) break;  /* safety */
                    }
                }
            }
        }
    }
    
    /* Check deleted count */
    int ndel = 0;
    for (uint32_t r = 0; r < t->row_count; r++)
        if (row_deleted(t, r)) ndel++;
    
    if (issues == 0) printf("  OK — %u rows (%d deleted), %u columns, indexes valid\n",
        t->row_count, ndel, t->num_columns);
    else printf("  %d issues found\n", issues);
    return issues;
}
/* ── BENCH — comprehensive multi-operation benchmark ── */
static int cmd_bench(int argc, char **argv) {
    int N = (argc > 1) ? atoi(argv[1]) : 10000;
    if (N < 10) N = 10;
    if (N > 100000) N = 100000;

    /* Create or reuse bench table */
    TableMeta *tbl = table_find("bench");
    if (!tbl) {
        /* Use mutable buffers — cmd_create modifies argv in place */
        char cbuf[6][256];
        strcpy(cbuf[0], "cmd"); strcpy(cbuf[1], "bench");
        strcpy(cbuf[2], "name:TEXT"); strcpy(cbuf[3], "age:INT");
        strcpy(cbuf[4], "city:TEXT"); strcpy(cbuf[5], "salary:FLOAT");
        char *cars[] = {cbuf[0], cbuf[1], cbuf[2], cbuf[3], cbuf[4], cbuf[5]};
        cmd_create(6, cars);
        tbl = table_find("bench");
        if (!tbl) { printf("Cannot create bench table\n"); return -1; }
    }

    /* Generate synthetic data if needed */
    const char *first[] = {"Abe","Aida","Bram","Cas","Dirk","Els","Finn","Gijs","Hans","Iris",
                           "Jan","Kees","Lars","Maud","Noor","Otto","Piet","Quint","Roos","Saar",
                           "Ties","Udo","Vera","Wim","Xavi","Yara","Zara","Anna","Bart","Cees"};
    const char *cities[] = {"Amsterdam","Rotterdam","Utrecht","DenHaag","Eindhoven","Groningen",
                            "Tilburg","Almere","Breda","Nijmegen","Arnhem","Haarlem","Leiden",
                            "Delft","Maastricht","Zwolle","Apeldoorn","Dordrecht","Leiden","Gent"};

    int exist = tbl->row_count;
    int need = N - exist;
    if (need < 0) need = 0;

    clock_t t0 = clock();

    if (need > 0) {
        fprintf(stderr, "Generating %d rows... ", need);
        for (int i = 0; i < need; i++) {
            char name[64], age[16], city[64], sal[32];
            int offset = (i + exist) * 7;
            snprintf(name, 64, "%s%d", first[(offset+0)%30], (offset+1)%100);
            snprintf(age, 16, "%d", 18 + (offset+2)%65);
            snprintf(city, 64, "%s", cities[(offset+3)%20]);
            snprintf(sal, 32, "%.0f", 25000.0 + (offset+4)%75000);
            char *vals[] = {"insert","bench",name,age,city,sal};
            cmd_insert(6, vals);
            if ((i+1) % 5000 == 0) meta_sync();
        }
        meta_sync();
        double imp = (double)(clock()-t0)/CLOCKS_PER_SEC;
        fprintf(stderr, "done (%.2fs, %.0f rows/s)\n", imp, need/imp);
    }

    ColumnMeta *c_name = column_find(tbl, "name");
    ColumnMeta *c_age  = column_find(tbl, "age");
    ColumnMeta *c_city = column_find(tbl, "city");
    ColumnMeta *c_sal  = column_find(tbl, "salary");
    if (!c_name || !c_age || !c_city || !c_sal) { printf("Missing columns\n"); return -1; }

    /* Build indexes */
    fprintf(stderr, "Building indexes... "); fflush(stderr);
    t0 = clock();
    if (c_age->sort_count != c_age->rows) sort_build(c_age);
    if (c_sal->sort_count != c_sal->rows) sort_build(c_sal);
    if (c_name->sort_count != c_name->rows) sort_build(c_name);
    if (c_city->sort_count != c_city->rows) sort_build(c_city);
    if (c_name->fzy_pages == 0) fzy_build(c_name);
    if (c_city->fzy_pages == 0) fzy_build(c_city);
    hash_rebuild(c_age); hash_rebuild(c_name); hash_rebuild(c_sal);
    double idx_sec = (double)(clock()-t0)/CLOCKS_PER_SEC;
    fprintf(stderr, "done (%.3fs)\n\n", idx_sec);

    /* Load table for direct access */
    tbl = table_find("bench");  /* refresh after mmap remap */
    c_name = column_find(tbl, "name");
    c_age  = column_find(tbl, "age");
    c_city = column_find(tbl, "city");
    c_sal  = column_find(tbl, "salary");

    int rows = tbl->row_count;
    printf("╔════════════════════════════════════════════════════════╗\n");
    printf("║  fastdb v%d BENCH  —  %d rows, %u columns        ║\n", VERSION, rows, tbl->num_columns);
    printf("╠════════════════════════════════════════════════════════╣\n");

    int iters = 5000000;
    {   clock_t t = clock(); volatile int s = 0;
        for (int i=0; i<iters; i++) s += int_at(c_age, i % rows);
        double dt = (double)(clock()-t)/CLOCKS_PER_SEC;
        printf("║ INT read (columnar)   %8d ops: %8.3fs → %9.1f ns/op ║\n", iters, dt, dt*1e9/iters);
    }

    {   int v = 42; uint32_t found[MAX_RES]; iters = 500000;
        clock_t t = clock(); volatile int s = 0;
        for (int i=0; i<iters; i++) { hash_lookup(c_age, &v, 4, found, 1); s += found[0]; }
        double dt = (double)(clock()-t)/CLOCKS_PER_SEC;
        printf("║ Hash lookup (INT)     %8d ops: %8.3fs → %9.1f ns/op ║\n", iters, dt, dt*1e9/iters);
    }

    {   iters = 500000;
        clock_t t = clock(); volatile int s = 0;
        for (int i=0; i<iters; i++) {
            uint32_t si = sort_lower_bound(c_age, 30.0);
            if (si < c_age->sort_count) { SortEntry *e = (SortEntry*)(db_map + c_age->sort_offset); s += e[si].row; }
        }
        double dt = (double)(clock()-t)/CLOCKS_PER_SEC;
        printf("║ Range lower_bound     %8d ops: %8.3fs → %9.1f ns/op ║\n", iters, dt, dt*1e9/iters);
    }

    {   iters = 50000;
        clock_t t = clock(); volatile int s = 0;
        for (int i=0; i<iters; i++) {
            uint32_t si = sort_lower_bound(c_age, 25.0);
            SortEntry *e = (SortEntry*)(db_map + c_age->sort_offset);
            int got = 0;
            while (si < c_age->sort_count && got < 50) {
                double v = (c_age->type == COL_INT) ? (double)e[si].ival : e[si].fval;
                if (v > 50.0) break;
                s += e[si].row; si++; got++;
            }
        }
        double dt = (double)(clock()-t)/CLOCKS_PER_SEC;
        printf("║ Range scan (50 rows)  %8d ops: %8.3fs → %9.1f µs/op ║\n", iters, dt, dt*1e6/iters);
    }

    {   const char *q = "Jan5"; iters = 500000;
        clock_t t = clock(); volatile int s = 0;
        for (int i=0; i<iters; i++) { uint32_t f[MAX_RES]; hash_lookup(c_name, q, 4, f, 1); s += f[0]; }
        double dt = (double)(clock()-t)/CLOCKS_PER_SEC;
        printf("║ Hash lookup (TEXT)    %8d ops: %8.3fs → %9.1f ns/op ║\n", iters, dt, dt*1e9/iters);
    }

    {   const char *pfx = "Jan"; iters = 10000;
        clock_t t = clock(); volatile int s = 0;
        int plen = strlen(pfx);
        for (int i=0; i<iters; i++) {
            uint32_t lo = text_lower_bound(c_name, pfx, plen);
            uint32_t hi = text_upper_bound(c_name, pfx, plen);
            s += (int)(hi - lo);
        }
        double dt = (double)(clock()-t)/CLOCKS_PER_SEC;
        printf("║ Prefix (text idx)     %8d ops: %8.3fs → %9.1f ns/op ║\n", iters, dt, dt*1e9/iters);
    }

    {   const char *ss = "dam"; iters = 10000;
        clock_t t = clock(); volatile int s = 0;
        for (int i=0; i<iters; i++) {
            uint32_t found2[MAX_RES];
            s += ss_search(c_city, ss, found2, MAX_RES);
        }
        double dt = (double)(clock()-t)/CLOCKS_PER_SEC;
        printf("║ Substring (trigram)   %8d ops: %8.3fs → %9.1f µs/op ║\n", iters, dt, dt*1e6/iters);
    }

    {   const char *q = "Amsterdm";  /* typo for Amsterdam */
        iters = 200;
        clock_t t = clock(); volatile int s = 0;
        for (int i=0; i<iters; i++) {
            uint32_t found2[MAX_RES];
            s += fzy_search(c_city, q, 2, found2, MAX_RES);
        }
        double dt = (double)(clock()-t)/CLOCKS_PER_SEC;
        printf("║ Fuzzy search (ed≤2)   %8d ops: %8.3fs → %9.1f µs/op ║\n", iters, dt, dt*1e6/iters);
    }

    {   iters = 5000000;
        clock_t t = clock(); volatile double s = 0;
        for (int i=0; i<iters; i++) s += float_at(c_sal, i % rows);
        double dt = (double)(clock()-t)/CLOCKS_PER_SEC;
        printf("║ FLOAT read (columnar) %8d ops: %8.3fs → %9.1f ns/op ║\n", iters, dt, dt*1e9/iters);
    }

    {   iters = 50000;
        clock_t t = clock();
        for (int i=0; i<iters; i++) {
            char name[64], age[16], city[64], sal[32];
            snprintf(name, 64, "B%04d", i);
            snprintf(age, 16, "%d", 20+(i%60));
            snprintf(city, 64, "%s", cities[i%20]);
            snprintf(sal, 32, "%d", 30000+i%70000);
            char *vals[] = {"insert","bench",name,age,city,sal};
            cmd_insert(6, vals);
        }
        meta_sync();
        double dt = (double)(clock()-t)/CLOCKS_PER_SEC;
        printf("║ Insert (no sync)      %8d ops: %8.3fs → %9.1f ns/op ║\n", iters, dt, dt*1e9/iters);
    }

    /* Memory footprint */
    size_t total_mmap = db_size;
    int active_rows = tbl->row_count;
    int bytes_per_row = active_rows ? (int)(total_mmap / active_rows) : 0;

    printf("╠════════════════════════════════════════════════════════╣\n");
    printf("║  MEMORY: %zu MB mmap, %d rows → %d bytes/row          ║\n",
           total_mmap/(1024*1024), active_rows, bytes_per_row);
    printf("╚════════════════════════════════════════════════════════╝\n");

    return 0;
}

static int cmd_serve(void) {
    char line[PAGE_SIZE];
    printf("fastdb> "); fflush(stdout);
    while (fgets(line, sizeof(line), stdin)) {
        int len = strlen(line);
        while (len>0 && (line[len-1]=='\n'||line[len-1]=='\r')) line[--len]=0;
        if (len==0) { printf("fastdb> "); fflush(stdout); continue; }
        
        char *tokens[32]; int nt=0;
        char *tok = strtok(line, " ");
        while (tok && nt<32) { tokens[nt++]=tok; tok=strtok(NULL," "); }
        if (nt==0) continue;
        
        if (!strcmp(tokens[0],"exit")||!strcmp(tokens[0],"quit")) break;
        else if (!strcmp(tokens[0],"create")) cmd_create(nt, tokens);
        else if (!strcmp(tokens[0],"alter"))  cmd_alter(nt, tokens);
        else if (!strcmp(tokens[0],"import")) cmd_import(nt, tokens);
        else if (!strcmp(tokens[0],"delete")) cmd_delete(nt, tokens);
        else if (!strcmp(tokens[0],"insert")) { cmd_insert(nt, tokens); meta_sync(); printf("ok\n"); }
        else if (!strcmp(tokens[0],"select")) cmd_select(nt, tokens);
        else if (!strcmp(tokens[0],"tables")) cmd_tables();
        else if (!strcmp(tokens[0],"verify")) cmd_verify(nt, tokens);
        else if (!strcmp(tokens[0],"bench"))  cmd_bench(nt, tokens);
        else if (!strcmp(tokens[0],"komma") && nt>=2) {
            int64_t n = atoll(tokens[1]);
            int64_t r = (n - 1) / 9;
            int c = (int)((n - 1) % 9);
            const char *safe_str = (c==0||c==1||c==3||c==4||c==6||c==7) ? "SAFE (prime candidate)" : "UNSAFE (÷3)";
            printf("%lld = 9·%lld + %d + 1  |  %lld/9 = %lld.%d  |  col=%d  %s\n",
                   (long long)n, (long long)r, c, (long long)n, (long long)r, c+1, c, safe_str);
        }
        else if (!strcmp(tokens[0],"explain")) {
            printf("Hash indexes: PlumHash ARX (39.1%% avalanche), O(1) equality. Sorted: per-column, O(log n) range. Fuzzy: trigram + Myers + Damerau.\n");
        }
        else printf("? %s\n", tokens[0]);
        fflush(stdout);
        printf("fastdb> "); fflush(stdout);
    }
    printf("bye\n");
    return 0;
}

/* ── Main ── */
int main(int argc, char **argv) {
    if (argc < 2) {
        printf("fastdb v1 — world's fastest columnar database\n\n"
               "  create TABLE col:TYPE ...\n"
               "  alter  TABLE add COL:TYPE\n"
               "  alter  TABLE drop COL\n"
               "  insert TABLE val1 val2 ...\n"
               "  select TABLE [where COL = VAL] [range COL LO HI]\n"
               "                [search COL PAT] [fuzzy COL PAT [K]] [prefix COL PAT]\n"
               "  delete TABLE where COL = VAL\n"
               "  import TABLE FILE\n"
               "  tables\n"
               "  verify TABLE\n"
               "  bench [N]\n"
               "  komma N\n"
               "  serve\n");
        return 0;
    }
    
    if (db_open(DB_FILE) < 0) { fprintf(stderr, "Cannot open %s\n", DB_FILE); return 1; }
    
    const char *cmd = argv[1];
    int ret = 0;
    
    if (!strcmp(cmd, "create")) ret = cmd_create(argc - 1, argv + 1);
    else if (!strcmp(cmd, "alter"))  ret = cmd_alter(argc - 1, argv + 1);
    else if (!strcmp(cmd, "import")) ret = cmd_import(argc - 1, argv + 1);
    else if (!strcmp(cmd, "delete")) ret = cmd_delete(argc - 1, argv + 1);
    else if (!strcmp(cmd, "insert")) { ret = cmd_insert(argc - 1, argv + 1); meta_sync(); if (ret == 0) printf("ok\n"); }
    else if (!strcmp(cmd, "select")) ret = cmd_select(argc - 1, argv + 1);
    else if (!strcmp(cmd, "tables")) ret = cmd_tables();
    else if (!strcmp(cmd, "verify")) ret = cmd_verify(argc - 1, argv + 1);
    else if (!strcmp(cmd, "serve"))  ret = cmd_serve();
    else if (!strcmp(cmd, "bench"))  ret = cmd_bench(argc - 1, argv + 1);
    else if (!strcmp(cmd, "komma") && argc >= 3) {
        int64_t n = atoll(argv[2]);
        int64_t r = (n - 1) / 9;
        int c = (int)((n - 1) % 9);
        const char *safe_str = (c==0||c==1||c==3||c==4||c==6||c==7) ? "SAFE (prime candidate)" : "UNSAFE (÷3)";
        printf("%lld = 9·%lld + %d + 1  |  %lld/9 = %lld.%d  |  col=%d  %s\n",
               (long long)n, (long long)r, c, (long long)n, (long long)r, c+1, c, safe_str);
    }
    else { fprintf(stderr, "? %s\n", cmd); ret = 1; }
    
    db_close();
    return ret;
}
