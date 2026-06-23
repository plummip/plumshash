/*
 * pe.c — Project Editor implementation
 *
 * Architecture:
 *   ┌─────────────┐     ┌──────────────────┐
 *   │  pe_edit_*   │────▶│  Scheduler       │
 *   │  (user API)  │     │  ┌────────────┐  │
 *   └─────────────┘     │  │ ready queue │  │
 *                        │  └─────┬──────┘  │
 *                        │        │ dequeue │
 *                        │  ┌─────▼──────┐  │
 *   ┌─────────────┐     │  │  workers    │  │
 *   │  pe_cache_*  │────▶│  │  (N threads)│  │
 *   │  pe_file_*   │     │  └─────┬──────┘  │
 *   └─────────────┘     │        │ apply   │
 *                        │  ┌─────▼──────┐  │
 *                        │  │ file cache  │  │
 *                        │  │ (hash tbl)  │  │
 *                        │  └────────────┘  │
 *                        └──────────────────┘
 *
 * Locking order (strict):
 *   1. pe->sched_lock   — protects ready queue, DAG state, hash table
 *   2. file->lock       — protects file data/line-index/dirty
 *
 * Never acquire sched_lock while holding a file lock.
 */

#define _GNU_SOURCE
#include "pe.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdint.h>
#include <unistd.h>
#include <sys/stat.h>
#include <errno.h>

/* ═══════════════════════════════════════════════════════════
 * Internal Types
 * ═══════════════════════════════════════════════════════════ */

/* A cached file */
typedef struct pe_file_s {
    char           *path;
    char           *data;
    size_t          size;
    char           *original;       /* snapshot at cache-load time   */
    size_t          original_size;
    size_t         *lines;
    size_t          nlines;
    _Atomic int     dirty;

    /* Undo stack */
    struct pe_undo_s {
        pe_op_t  op;
        size_t   start_off, end_off;
        char    *old_content;
        size_t   old_content_len;
    } *undo_stack;
    size_t          undo_depth;
    size_t          undo_cap;

    pthread_rwlock_t lock;
} pe_file_t;

/* An edit operation */
struct pe_edit_s {
    pe_t        *pe;            /* back-pointer to editor             */
    char        *path;          /* relative file path (owned)         */
    pe_file_t   *file;          /* resolved at submit time            */
    pe_op_t      op;
    pe_pos_t     start, end;
    char        *content;       /* for INSERT/REPLACE (owned)         */
    size_t       content_len;

    /* DAG — all access under pe->sched_lock */
    size_t       ndeps;
    size_t       deps_remaining;
    pe_edit_t  **rdeps;         /* reverse: edits that depend on this */
    size_t       nrdeps;
    size_t       nrdeps_cap;

    /* State (atomic for lock-free reads from pe_edit_state) */
    _Atomic pe_edit_state_t state;
    char        *error;         /* set on failure (owned)             */

    /* Links */
    pe_edit_t   *qnext;         /* ready-queue link                   */
    pe_edit_t   *anext;         /* all-edits list link                */
};

/* The editor itself */
struct pe_s {
    char  *root;                /* project root (no trailing /)       */
    size_t root_len;

    /* File cache — open-addressing hash table (string → pe_file_t*)  */
    pe_file_t **files;
    size_t      files_cap;
    size_t      files_count;
    pe_file_t  *tombstone;      /* sentinel for deleted slots         */

    /* Thread pool */
    pthread_t *workers;
    size_t     nworkers;
    bool       running;         /* false → workers exit               */

    /* Ready queue (FIFO, singly-linked) */
    pe_edit_t *ready_head;
    pe_edit_t *ready_tail;

    /* Synchronization */
    pthread_mutex_t sched_lock;
    pthread_cond_t  sched_cond;   /* workers wait for work            */
    pthread_cond_t  done_cond;    /* flush() waits for pending==0     */

    /* Counters (protected by sched_lock) */
    size_t pending;
    size_t nfailed;

    /* Transaction state (protected by sched_lock) */
    bool        txn_active;
    pe_edit_t **txn_edits;
    size_t      txn_nedits;
    size_t      txn_cap;

    /* Branch storage */
    struct pe_branch_s {
        char        *name;
        pe_file_t  **files;       /* hash table for this branch      */
        size_t       files_cap;
        size_t       files_count;
        pe_file_t   *tombstone;
    } *branches;
    size_t      nbranches;
    size_t      branches_cap;
    char       *current_branch;   /* NULL = "main" (default)         */

    /* All-edits list (for cleanup in pe_destroy) */
    pe_edit_t *all_edits;
};

/* ═══════════════════════════════════════════════════════════
 * Hash Table (open-addressing, FNV-1a, linear probing)
 * ═══════════════════════════════════════════════════════════ */

#define HT_INIT_CAP  64
#define HT_LOAD_PCT  70

static uint64_t ht_hash(const char *s) {
    uint64_t h = 14695981039346656037ULL;
    for (; *s; s++) {
        h ^= (uint64_t)(unsigned char)*s;
        h *= 1099511628211ULL;
    }
    return h;
}

static int ht_grow(pe_t *pe) {
    size_t old_cap = pe->files_cap;
    pe_file_t **old = pe->files;
    size_t new_cap = (old_cap == 0) ? HT_INIT_CAP : old_cap * 2;
    pe_file_t **new_tbl = calloc(new_cap, sizeof(pe_file_t *));
    if (!new_tbl) return -1;

    for (size_t i = 0; i < old_cap; i++) {
        pe_file_t *e = old[i];
        if (!e || e == pe->tombstone) continue;
        uint64_t h = ht_hash(e->path);
        size_t idx = h % new_cap;
        while (new_tbl[idx]) idx = (idx + 1) % new_cap;
        new_tbl[idx] = e;
    }

    free(old);
    pe->files     = new_tbl;
    pe->files_cap = new_cap;
    return 0;
}

/* Lookup — caller must hold sched_lock */
static pe_file_t *ht_find(pe_t *pe, const char *path) {
    if (pe->files_cap == 0) return NULL;
    uint64_t h = ht_hash(path);
    size_t idx = h % pe->files_cap;
    size_t orig = idx;
    do {
        pe_file_t *e = pe->files[idx];
        if (!e) return NULL;               /* empty slot → not found  */
        if (e != pe->tombstone && strcmp(e->path, path) == 0) return e;
        idx = (idx + 1) % pe->files_cap;
    } while (idx != orig);
    return NULL;
}

/* Insert — caller must hold sched_lock.  Returns -1 on duplicate. */
static int ht_insert(pe_t *pe, pe_file_t *entry) {
    if (pe->files_cap == 0 ||
        pe->files_count * 100 >= pe->files_cap * HT_LOAD_PCT) {
        if (ht_grow(pe) != 0) return -1;
    }
    uint64_t h = ht_hash(entry->path);
    size_t idx = h % pe->files_cap;
    while (pe->files[idx] && pe->files[idx] != pe->tombstone) {
        if (strcmp(pe->files[idx]->path, entry->path) == 0) return -1;
        idx = (idx + 1) % pe->files_cap;
    }
    pe->files[idx] = entry;
    pe->files_count++;
    return 0;
}

/* ═══════════════════════════════════════════════════════════
 * Path Helpers
 * ═══════════════════════════════════════════════════════════ */

static char *pe_mkfull(pe_t *pe, const char *rel, size_t *len) {
    size_t rl = strlen(rel);
    size_t total = pe->root_len + 1 + rl;
    char *full = malloc(total + 1);
    if (!full) return NULL;
    memcpy(full, pe->root, pe->root_len);
    full[pe->root_len] = '/';
    memcpy(full + pe->root_len + 1, rel, rl);
    full[total] = '\0';
    if (len) *len = total;
    return full;
}

/* ═══════════════════════════════════════════════════════════
 * File I/O (disk ↔ memory)
 * ═══════════════════════════════════════════════════════════ */

static int file_read(pe_t *pe, const char *rel,
                     char **data, size_t *size) {
    size_t flen;
    char *full = pe_mkfull(pe, rel, &flen);
    if (!full) return -1;

    FILE *f = fopen(full, "rb");
    free(full);
    if (!f) return -1;

    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return -1; }
    long sz = ftell(f);
    if (sz < 0) { fclose(f); return -1; }
    rewind(f);

    char *buf = malloc((size_t)sz + 1);
    if (!buf) { fclose(f); return -1; }

    size_t n = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    if (n != (size_t)sz) { free(buf); return -1; }

    buf[sz] = '\0';
    *data = buf;
    *size = (size_t)sz;
    return 0;
}

/* ═══════════════════════════════════════════════════════════
 * Line Index
 * ═══════════════════════════════════════════════════════════ */

static int build_lines(char *data, size_t size,
                       size_t **lines, size_t *nlines) {
    /* Count newlines to get raw line count */
    size_t nl = 1;
    for (size_t i = 0; i < size; i++)
        if (data[i] == '\n') nl++;

    /* Trailing \n does not create an extra line (match wc -l) */
    if (size > 0 && data[size - 1] == '\n')
        nl--;
    if (nl == 0) nl = 1;  /* purely empty file */

    size_t *l = malloc(nl * sizeof(size_t));
    if (!l) return -1;

    l[0] = 0;
    size_t li = 1;
    for (size_t i = 0; i < size; i++) {
        if (data[i] == '\n' && li < nl)
            l[li++] = i + 1;
    }

    *lines  = l;
    *nlines = nl;
    return 0;
}

/* ═══════════════════════════════════════════════════════════
 * Position Resolver
 * ═══════════════════════════════════════════════════════════ */

/* Convert (line, col) to byte offset.  Returns SIZE_MAX on error.
   line=0       → byte 0
   line=SIZE_MAX → file->size (end of file)
   col=0        → start of line (before first column)           */
static size_t resolve_pos(pe_file_t *f, pe_pos_t pos) {
    if (pos.line == 0)          return 0;
    if (pos.line == SIZE_MAX)   return f->size;
    if (pos.line > f->nlines)   return SIZE_MAX;

    size_t line_start = f->lines[pos.line - 1];

    /* length of this line excluding the \n */
    size_t line_len;
    if (pos.line < f->nlines) {
        line_len = f->lines[pos.line] - line_start;
        if (line_len > 0 && f->data[f->lines[pos.line] - 1] == '\n')
            line_len--;
    } else {
        line_len = f->size - line_start;
        if (line_len > 0 && f->data[f->size - 1] == '\n')
            line_len--;
    }

    if (pos.col == 0)          return line_start;
    if (pos.col > line_len + 1) return SIZE_MAX; /* past end of line */
    return line_start + pos.col - 1;
}

/* ═══════════════════════════════════════════════════════════
 * Edit Application (on in-memory buffer)
 * ═══════════════════════════════════════════════════════════ */

/* All applicators assume caller holds file->lock (write). */

static int apply_insert(pe_file_t *f, size_t off,
                        const char *content, size_t len) {
    if (off > f->size) return -1;
    char *nd = realloc(f->data, f->size + len + 1);
    if (!nd) return -1;
    f->data = nd;
    memmove(f->data + off + len, f->data + off, f->size - off);
    memcpy(f->data + off, content, len);
    f->size += len;
    f->data[f->size] = '\0';
    atomic_store(&f->dirty, 1);

    free(f->lines);
    return build_lines(f->data, f->size, &f->lines, &f->nlines);
}

static int apply_delete(pe_file_t *f, size_t start, size_t end) {
    if (start > f->size || end > f->size || start > end) return -1;
    size_t len = end - start;
    if (len == 0) return 0;
    memmove(f->data + start, f->data + end, f->size - end);
    f->size -= len;
    f->data[f->size] = '\0';
    atomic_store(&f->dirty, 1);

    free(f->lines);
    return build_lines(f->data, f->size, &f->lines, &f->nlines);
}

static int apply_replace(pe_file_t *f, size_t start, size_t end,
                         const char *content, size_t len) {
    if (start > f->size || end > f->size || start > end) return -1;
    size_t oldlen = end - start;
    ssize_t delta = (ssize_t)len - (ssize_t)oldlen;
    if (delta > 0) {
        char *nd = realloc(f->data, f->size + (size_t)delta + 1);
        if (!nd) return -1;
        f->data = nd;
    }
    if (delta != 0)
        memmove(f->data + start + len, f->data + end, f->size - end);
    memcpy(f->data + start, content, len);
    f->size = f->size - oldlen + len;
    f->data[f->size] = '\0';
    atomic_store(&f->dirty, 1);
    free(f->lines);
    return build_lines(f->data, f->size, &f->lines, &f->nlines);
}

/* ═══════════════════════════════════════════════════════════
 * Scheduler Internals
 * ═══════════════════════════════════════════════════════════ */

/* Push an edit onto the ready queue — caller holds sched_lock. */
static void ready_push(pe_t *pe, pe_edit_t *edit) {
    edit->state   = PE_EDIT_READY;
    edit->qnext   = NULL;
    if (pe->ready_tail)
        pe->ready_tail->qnext = edit;
    else
        pe->ready_head = edit;
    pe->ready_tail = edit;
}

/* Called with sched_lock held when an edit finishes.
   Wakes dependents; if any become ready, pushes to queue. */
static void complete_edit(pe_t *pe, pe_edit_t *edit) {
    if (edit->error) {
        edit->state = PE_EDIT_FAILED;
        pe->nfailed++;
    } else {
        edit->state = PE_EDIT_DONE;
    }
    pe->pending--;

    /* Satisfy reverse dependencies */
    for (size_t i = 0; i < edit->nrdeps; i++) {
        pe_edit_t *rdep = edit->rdeps[i];
        if (rdep->deps_remaining == 0) continue; /* already ready */
        rdep->deps_remaining--;
        if (rdep->deps_remaining == 0 && rdep->file != NULL)
            ready_push(pe, rdep);
    }

    if (pe->pending == 0)
        pthread_cond_broadcast(&pe->done_cond);
}

/* Apply an edit to its target file.  Called by worker thread. */
static void apply_edit(pe_edit_t *edit) {
    pe_file_t *f = edit->file;

    pthread_rwlock_wrlock(&f->lock);

    size_t off_start = resolve_pos(f, edit->start);
    if (off_start == SIZE_MAX) {
        edit->error = strdup("invalid start position");
        pthread_rwlock_unlock(&f->lock);
        return;
    }

    size_t off_end = 0;
    if (edit->op != PE_OP_INSERT) {
        if (edit->end.line == 0 && edit->end.col == 0)
            off_end = f->size;          /* "to end of file" shorthand */
        else {
            off_end = resolve_pos(f, edit->end);
            if (off_end == SIZE_MAX) {
                edit->error = strdup("invalid end position");
                pthread_rwlock_unlock(&f->lock);
                return;
            }
        }
    }

    /* ── Capture old content for undo ── */
    char   *old_data = NULL;
    size_t  old_len  = 0;

    if (edit->op == PE_OP_DELETE || edit->op == PE_OP_REPLACE) {
        old_len = off_end - off_start;
        if (old_len > 0) {
            old_data = malloc(old_len);
            if (old_data) memcpy(old_data, f->data + off_start, old_len);
        }
    } else if (edit->op == PE_OP_INSERT) {
        /* For undo: we need to know what was at the insertion point
           (nothing — the undo will delete the inserted range) */
        old_len = 0;
    }

    int rc = 0;
    switch (edit->op) {
    case PE_OP_INSERT:
        rc = apply_insert(f, off_start, edit->content, edit->content_len);
        break;
    case PE_OP_DELETE:
        rc = apply_delete(f, off_start, off_end);
        break;
    case PE_OP_REPLACE:
        rc = apply_replace(f, off_start, off_end,
                           edit->content, edit->content_len);
        break;
    }

    if (rc != 0) {
        free(old_data);
        edit->error = strdup("edit application failed");
    } else {
        /* ── Record undo entry ── */
        if (f->undo_depth >= f->undo_cap) {
            size_t nc = f->undo_cap ? f->undo_cap * 2 : 16;
            void *ns = realloc(f->undo_stack, nc * sizeof(*f->undo_stack));
            if (ns) {
                f->undo_stack = ns;
                f->undo_cap   = nc;
            }
        }
        if (f->undo_depth < f->undo_cap) {
            struct pe_undo_s *u = &f->undo_stack[f->undo_depth++];
            u->op             = edit->op;
            u->start_off      = off_start;
            u->end_off        = (edit->op == PE_OP_INSERT)
                                ? off_start + edit->content_len : off_end;
            u->old_content    = old_data;
            u->old_content_len = old_len;
            old_data = NULL;  /* ownership transferred */
        }
        free(old_data);
    }

    pthread_rwlock_unlock(&f->lock);
}

/* ── Worker thread ─────────────────────────────────────── */

static void *worker_main(void *arg) {
    pe_t *pe = (pe_t *)arg;

    pthread_mutex_lock(&pe->sched_lock);
    for (;;) {
        while (pe->ready_head == NULL && pe->running)
            pthread_cond_wait(&pe->sched_cond, &pe->sched_lock);

        if (!pe->running && pe->ready_head == NULL)
            break;

        /* Dequeue */
        pe_edit_t *edit = pe->ready_head;
        pe->ready_head  = edit->qnext;
        if (!pe->ready_head) pe->ready_tail = NULL;
        edit->qnext = NULL;
        edit->state = PE_EDIT_RUNNING;

        pthread_mutex_unlock(&pe->sched_lock);

        /* ── do the work (no locks held) ── */
        apply_edit(edit);

        /* ── complete (re-acquire) ── */
        pthread_mutex_lock(&pe->sched_lock);
        complete_edit(pe, edit);
    }
    pthread_mutex_unlock(&pe->sched_lock);
    return NULL;
}

/* ═══════════════════════════════════════════════════════════
 * Public API
 * ═══════════════════════════════════════════════════════════ */

pe_t *pe_create(const char *root, size_t nworkers) {
    pe_t *pe = calloc(1, sizeof(pe_t));
    if (!pe) return NULL;

    /* Normalize root: strip trailing slashes */
    size_t rl = strlen(root);
    while (rl > 0 && root[rl - 1] == '/') rl--;
    pe->root = malloc(rl + 1);
    if (!pe->root) { free(pe); return NULL; }
    memcpy(pe->root, root, rl);
    pe->root[rl] = '\0';
    pe->root_len = rl;

    /* Hash-table tombstone */
    pe->tombstone = calloc(1, sizeof(pe_file_t));
    if (!pe->tombstone) { free(pe->root); free(pe); return NULL; }

    /* Thread-pool size */
    if (nworkers == 0) {
        long n = sysconf(_SC_NPROCESSORS_ONLN);
        pe->nworkers = (n > 0) ? (size_t)n : 4;
    } else {
        pe->nworkers = nworkers;
    }

    /* Synchronization primitives */
    if (pthread_mutex_init(&pe->sched_lock, NULL) != 0 ||
        pthread_cond_init(&pe->sched_cond, NULL)  != 0 ||
        pthread_cond_init(&pe->done_cond, NULL)   != 0) {
        free(pe->tombstone); free(pe->root); free(pe);
        return NULL;
    }

    /* Spawn workers */
    pe->running  = true;
    pe->workers  = malloc(pe->nworkers * sizeof(pthread_t));
    if (!pe->workers) {
        pthread_cond_destroy(&pe->done_cond);
        pthread_cond_destroy(&pe->sched_cond);
        pthread_mutex_destroy(&pe->sched_lock);
        free(pe->tombstone); free(pe->root); free(pe);
        return NULL;
    }

    size_t i;
    for (i = 0; i < pe->nworkers; i++) {
        if (pthread_create(&pe->workers[i], NULL, worker_main, pe) != 0)
            break;
    }
    if (i < pe->nworkers) {
        /* Partial failure — stop the ones we started */
        pthread_mutex_lock(&pe->sched_lock);
        pe->running = false;
        pthread_cond_broadcast(&pe->sched_cond);
        pthread_mutex_unlock(&pe->sched_lock);
        for (size_t j = 0; j < i; j++)
            pthread_join(pe->workers[j], NULL);
        free(pe->workers);
        pthread_cond_destroy(&pe->done_cond);
        pthread_cond_destroy(&pe->sched_cond);
        pthread_mutex_destroy(&pe->sched_lock);
        free(pe->tombstone); free(pe->root); free(pe);
        return NULL;
    }

    return pe;
}

/* Forward declaration */
static void branch_free_files(struct pe_branch_s *br);

void pe_destroy(pe_t *pe) {
    if (!pe) return;

    /* Wait for everything to finish */
    pe_flush(pe);

    /* Stop workers */
    pthread_mutex_lock(&pe->sched_lock);
    pe->running = false;
    pthread_cond_broadcast(&pe->sched_cond);
    pthread_mutex_unlock(&pe->sched_lock);

    for (size_t i = 0; i < pe->nworkers; i++)
        pthread_join(pe->workers[i], NULL);
    free(pe->workers);

    /* Free all edits */
    pe_edit_t *e = pe->all_edits;
    while (e) {
        pe_edit_t *next = e->anext;
        free(e->path);
        free(e->content);
        free(e->rdeps);
        free(e->error);
        free(e);
        e = next;
    }

    /* Free file cache */
    for (size_t i = 0; i < pe->files_cap; i++) {
        pe_file_t *f = pe->files[i];
        if (!f || f == pe->tombstone) continue;
        pthread_rwlock_destroy(&f->lock);
        free(f->path);
        free(f->data);
        free(f->original);
        free(f->lines);
        if (f->undo_stack) {
            for (size_t j = 0; j < f->undo_depth; j++)
                free(f->undo_stack[j].old_content);
        }
        free(f->undo_stack);
        free(f);
    }
    free(pe->files);
    free(pe->tombstone);

    /* Free transaction state */
    free(pe->txn_edits);

    /* Free branches */
    for (size_t i = 0; i < pe->nbranches; i++) {
        free(pe->branches[i].name);
        branch_free_files(&pe->branches[i]);
    }
    free(pe->branches);
    free(pe->current_branch);

    /* Destroy sync primitives */
    pthread_cond_destroy(&pe->done_cond);
    pthread_cond_destroy(&pe->sched_cond);
    pthread_mutex_destroy(&pe->sched_lock);

    free(pe->root);
    free(pe);
}

int pe_cache_file(pe_t *pe, const char *path) {
    pthread_mutex_lock(&pe->sched_lock);
    pe_file_t *f = ht_find(pe, path);
    pthread_mutex_unlock(&pe->sched_lock);
    if (f) return 0;  /* already cached */

    /* Allocate + load (no lock — disk I/O) */
    f = calloc(1, sizeof(pe_file_t));
    if (!f) return -1;
    f->path = strdup(path);
    if (!f->path) { free(f); return -1; }

    if (file_read(pe, path, &f->data, &f->size) != 0)
        { free(f->path); free(f); return -1; }
    if (build_lines(f->data, f->size, &f->lines, &f->nlines) != 0)
        { free(f->data); free(f->path); free(f); return -1; }

    /* Snapshot original for diff/undo */
    f->original = malloc(f->size + 1);
    if (f->original) {
        memcpy(f->original, f->data, f->size);
        f->original[f->size] = '\0';
        f->original_size = f->size;
    }

    pthread_rwlock_init(&f->lock, NULL);
    atomic_init(&f->dirty, 0);

    /* Insert into cache (may race, which we handle) */
    pthread_mutex_lock(&pe->sched_lock);
    if (ht_insert(pe, f) != 0) {
        /* Another thread beat us — discard ours, use theirs */
        pthread_rwlock_destroy(&f->lock);
        free(f->lines); free(f->data); free(f->path); free(f);
        f = ht_find(pe, path);  /* must exist now */
        if (!f) {
            pthread_mutex_unlock(&pe->sched_lock);
            return -1;
        }
    }
    pthread_mutex_unlock(&pe->sched_lock);
    return 0;
}

pe_edit_t *pe_edit_create(pe_t *pe, const char *path, pe_op_t op,
                          pe_pos_t start, pe_pos_t end,
                          const char *content, size_t len) {
    pe_edit_t *e = calloc(1, sizeof(pe_edit_t));
    if (!e) return NULL;

    e->pe    = pe;
    e->path  = strdup(path);
    if (!e->path) { free(e); return NULL; }
    e->op    = op;
    e->start = start;
    e->end   = end;
    e->state = PE_EDIT_PENDING;

    if (content && len > 0) {
        e->content = malloc(len + 1);
        if (!e->content) { free(e->path); free(e); return NULL; }
        memcpy(e->content, content, len);
        e->content[len] = '\0';
        e->content_len  = len;
    }

    /* Link into all-edits list (for cleanup) */
    pthread_mutex_lock(&pe->sched_lock);
    e->anext       = pe->all_edits;
    pe->all_edits  = e;
    pthread_mutex_unlock(&pe->sched_lock);

    return e;
}

int pe_edit_depend(pe_edit_t *edit, pe_edit_t *dep) {
    pe_t *pe = edit->pe;

    pthread_mutex_lock(&pe->sched_lock);

    /* Grow dep->rdeps if needed */
    if (dep->nrdeps >= dep->nrdeps_cap) {
        size_t nc = dep->nrdeps_cap ? dep->nrdeps_cap * 2 : 4;
        pe_edit_t **nr = realloc(dep->rdeps, nc * sizeof(pe_edit_t *));
        if (!nr) { pthread_mutex_unlock(&pe->sched_lock); return -1; }
        dep->rdeps      = nr;
        dep->nrdeps_cap = nc;
    }
    dep->rdeps[dep->nrdeps++] = edit;
    edit->ndeps++;
    edit->deps_remaining++;

    /* If dep is already done, the edit would never be woken.
       Satisfy the dependency now. */
    pe_edit_state_t ds = atomic_load(&dep->state);
    if (ds == PE_EDIT_DONE || ds == PE_EDIT_FAILED) {
        edit->deps_remaining--;
        if (edit->deps_remaining == 0 && edit->file != NULL)
            ready_push(pe, edit);
    }

    pthread_mutex_unlock(&pe->sched_lock);
    return 0;
}

int pe_edit_submit(pe_t *pe, pe_edit_t *edit) {
    /* ── Resolve file (load if not cached) ── */
    pthread_mutex_lock(&pe->sched_lock);
    pe_file_t *f = ht_find(pe, edit->path);
    pthread_mutex_unlock(&pe->sched_lock);

    if (!f) {
        f = calloc(1, sizeof(pe_file_t));
        if (!f) return -1;
        f->path = strdup(edit->path);
        if (!f->path) { free(f); return -1; }

        if (file_read(pe, edit->path, &f->data, &f->size) != 0)
            { free(f->path); free(f); return -1; }
        if (build_lines(f->data, f->size, &f->lines, &f->nlines) != 0)
            { free(f->data); free(f->path); free(f); return -1; }

        /* Snapshot original for diff/undo */
        f->original = malloc(f->size + 1);
        if (f->original) {
            memcpy(f->original, f->data, f->size);
            f->original[f->size] = '\0';
            f->original_size = f->size;
        }

        pthread_rwlock_init(&f->lock, NULL);
        atomic_init(&f->dirty, 0);

        pthread_mutex_lock(&pe->sched_lock);
        if (ht_insert(pe, f) != 0) {
            pthread_rwlock_destroy(&f->lock);
            free(f->lines); free(f->data); free(f->path); free(f);
            f = ht_find(pe, edit->path);
            if (!f) { pthread_mutex_unlock(&pe->sched_lock); return -1; }
        }
        pthread_mutex_unlock(&pe->sched_lock);
    }

    edit->file = f;

    /* ── Queue the edit ── */
    pthread_mutex_lock(&pe->sched_lock);

    if (pe->txn_active) {
        /* Transaction mode: queue for later commit */
        if (pe->txn_nedits >= pe->txn_cap) {
            size_t nc = pe->txn_cap ? pe->txn_cap * 2 : 16;
            pe_edit_t **ne = realloc(pe->txn_edits, nc * sizeof(pe_edit_t *));
            if (!ne) {
                pthread_mutex_unlock(&pe->sched_lock);
                return -1;
            }
            pe->txn_edits = ne;
            pe->txn_cap   = nc;
        }
        pe->txn_edits[pe->txn_nedits++] = edit;
        /* Don't increment pending — txn_commit handles that */
    } else {
        pe->pending++;
        if (edit->deps_remaining == 0)
            ready_push(pe, edit);
        pthread_cond_signal(&pe->sched_cond);
    }

    pthread_mutex_unlock(&pe->sched_lock);

    return 0;
}

size_t pe_flush(pe_t *pe) {
    pthread_mutex_lock(&pe->sched_lock);
    while (pe->pending > 0)
        pthread_cond_wait(&pe->done_cond, &pe->sched_lock);
    size_t nf = pe->nfailed;
    pthread_mutex_unlock(&pe->sched_lock);
    return nf;
}

int pe_sync(pe_t *pe) {
    int errors = 0;

    for (size_t i = 0; i < pe->files_cap; i++) {
        pe_file_t *f = pe->files[i];
        if (!f || f == pe->tombstone) continue;

        /* Atomically test-and-clear dirty flag */
        if (!atomic_exchange(&f->dirty, 0))
            continue;  /* not dirty */

        /* Acquire read lock to get a stable snapshot of data/size.
           Writers are blocked only for the duration of the memcpy
           inside apply_*, so this read lock is very short-lived. */
        pthread_rwlock_rdlock(&f->lock);

        size_t flen;
        char *full = pe_mkfull(pe, f->path, &flen);
        if (!full) {
            pthread_rwlock_unlock(&f->lock);
            errors++;
            continue;
        }

        /* Snapshot data/size under lock, then release before I/O */
        size_t sz = f->size;
        char *snap = malloc(sz);
        if (!snap) {
            pthread_rwlock_unlock(&f->lock);
            free(full);
            errors++;
            continue;
        }
        memcpy(snap, f->data, sz);
        pthread_rwlock_unlock(&f->lock);

        /* Write to temp file, then rename (atomic on same FS) */
        size_t tmplen = flen + 5;
        char *tmp = malloc(tmplen);
        if (!tmp) { free(snap); free(full); errors++; continue; }
        snprintf(tmp, tmplen, "%s.tmp", full);

        FILE *fp = fopen(tmp, "wb");
        if (!fp) {
            free(tmp); free(snap); free(full);
            errors++;
            continue;
        }

        size_t written = fwrite(snap, 1, sz, fp);
        int ferr = ferror(fp);
        fclose(fp);

        if (written != sz || ferr) {
            unlink(tmp);
            free(tmp); free(snap); free(full);
            errors++;
            continue;
        }

        if (rename(tmp, full) != 0) {
            unlink(tmp);
            free(tmp); free(snap); free(full);
            errors++;
            continue;
        }

        free(tmp);
        free(full);

        /* Refresh original snapshot (brief write-lock for metadata) */
        pthread_rwlock_wrlock(&f->lock);
        free(f->original);
        f->original = snap;  /* reuse snapshot as new original */
        f->original_size = sz;
        pthread_rwlock_unlock(&f->lock);
    }

    return errors ? -1 : 0;
}

pe_edit_state_t pe_edit_state(pe_edit_t *edit) {
    return atomic_load(&edit->state);
}

const char *pe_edit_error(pe_edit_t *edit) {
    return edit->error ? edit->error : "no error";
}

void pe_edit_free(pe_edit_t *edit) {
    if (!edit) return;
    pe_t *pe = edit->pe;

    /* Unlink from all-edits */
    pthread_mutex_lock(&pe->sched_lock);
    if (pe->all_edits == edit) {
        pe->all_edits = edit->anext;
    } else {
        for (pe_edit_t *e = pe->all_edits; e; e = e->anext) {
            if (e->anext == edit) {
                e->anext = edit->anext;
                break;
            }
        }
    }
    pthread_mutex_unlock(&pe->sched_lock);

    free(edit->path);
    free(edit->content);
    free(edit->rdeps);
    free(edit->error);
    free(edit);
}

const char *pe_file_data(pe_t *pe, const char *path, size_t *size) {
    pthread_mutex_lock(&pe->sched_lock);
    pe_file_t *f = ht_find(pe, path);
    pthread_mutex_unlock(&pe->sched_lock);
    if (!f) {
        if (size) *size = 0;
        return NULL;
    }
    if (size) *size = f->size;
    return f->data;
}

size_t pe_file_lines(pe_t *pe, const char *path) {
    pthread_mutex_lock(&pe->sched_lock);
    pe_file_t *f = ht_find(pe, path);
    pthread_mutex_unlock(&pe->sched_lock);
    return f ? f->nlines : 0;
}

/* ═══════════════════════════════════════════════════════════
 * Undo
 * ═══════════════════════════════════════════════════════════ */

int pe_undo(pe_t *pe, const char *path) {
    pthread_mutex_lock(&pe->sched_lock);
    pe_file_t *f = ht_find(pe, path);
    pthread_mutex_unlock(&pe->sched_lock);
    if (!f) return -1;

    pthread_rwlock_wrlock(&f->lock);

    if (f->undo_depth == 0) {
        pthread_rwlock_unlock(&f->lock);
        return -1;
    }

    struct pe_undo_s *u = &f->undo_stack[--f->undo_depth];
    int rc = 0;

    switch (u->op) {
    case PE_OP_INSERT:
        /* Undo insert = delete the inserted range */
        rc = apply_delete(f, u->start_off, u->end_off);
        break;
    case PE_OP_DELETE:
        /* Undo delete = re-insert the old content */
        rc = apply_insert(f, u->start_off, u->old_content, u->old_content_len);
        break;
    case PE_OP_REPLACE:
        /* Undo replace = delete new content, re-insert old content */
        rc = apply_delete(f, u->start_off, u->end_off);
        if (rc == 0)
            rc = apply_insert(f, u->start_off, u->old_content, u->old_content_len);
        break;
    }

    free(u->old_content);
    u->old_content = NULL;

    pthread_rwlock_unlock(&f->lock);
    return rc;
}

size_t pe_undo_depth(pe_t *pe, const char *path) {
    pthread_mutex_lock(&pe->sched_lock);
    pe_file_t *f = ht_find(pe, path);
    pthread_mutex_unlock(&pe->sched_lock);
    return f ? f->undo_depth : 0;
}

/* ═══════════════════════════════════════════════════════════
 * Unified Diff
 * ═══════════════════════════════════════════════════════════ */

/* Split text into lines. Returns array of pointers into a copy. */
static char **split_lines(const char *data, size_t size, size_t *nlines) {
    size_t nl = 0;
    for (size_t i = 0; i < size; i++)
        if (data[i] == '\n') nl++;
    if (size > 0 && data[size - 1] != '\n') nl++;  /* last unterminated line */

    char **lines = malloc(nl * sizeof(char *));
    if (!lines) { *nlines = 0; return NULL; }

    size_t li = 0, start = 0;
    for (size_t i = 0; i < size; i++) {
        if (data[i] == '\n') {
            lines[li] = (char *)data + start;
            ((char *)data)[i] = '\0';  /* temporarily null-terminate */
            li++;
            start = i + 1;
        }
    }
    if (start < size) {
        lines[li] = (char *)data + start;
        li++;
    }
    *nlines = nl;
    return lines;
}

/* Simple unified-diff: walk both versions, emit hunks for differing regions.
   Not Myers-optimal but correct and readable. */
static char *diff_unified(const char *adata, size_t asize,
                           const char *bdata, size_t bsize,
                           const char *path, size_t ctx) {
    char *acopy = malloc(asize + 1);
    char *bcopy = malloc(bsize + 1);
    if (!acopy || !bcopy) { free(acopy); free(bcopy); return NULL; }
    memcpy(acopy, adata, asize); acopy[asize] = '\0';
    memcpy(bcopy, bdata, bsize); bcopy[bsize] = '\0';

    size_t alen, blen;
    char **a = split_lines(acopy, asize, &alen);
    char **b = split_lines(bcopy, bsize, &blen);
    if (!a || !b) { free(a); free(b); free(acopy); free(bcopy); return NULL; }

    /* Check if identical */
    if (alen == blen) {
        bool same = true;
        for (size_t i = 0; i < alen; i++)
            if (strcmp(a[i], b[i]) != 0) { same = false; break; }
        if (same) {
            free(a); free(b); free(acopy); free(bcopy);
            return strdup("");
        }
    }

    size_t cap = 4096;
    char *out = malloc(cap);
    if (!out) { free(a); free(b); free(acopy); free(bcopy); return NULL; }
    size_t ol = 0;
    #define O(...) do { \
        int _n = snprintf(out + ol, cap - ol, __VA_ARGS__); \
        if (_n < 0) _n = 0; \
        while (ol + (size_t)_n + 1 > cap) { \
            cap *= 2; char *_t = realloc(out, cap); \
            if (!_t) goto oom; out = _t; \
            _n = snprintf(out + ol, cap - ol, __VA_ARGS__); \
            if (_n < 0) _n = 0; \
        } \
        ol += (size_t)_n; \
    } while(0)

    O("--- a/%s\n+++ b/%s\n", path, path);

    size_t i = 0, j = 0;
    while (i < alen || j < blen) {
        /* Skip matching prefix */
        while (i < alen && j < blen && strcmp(a[i], b[j]) == 0)
            { i++; j++; }
        if (i >= alen && j >= blen) break;

        /* Find a re-sync point: look ahead up to 64 lines for a match */
        size_t best_a = alen, best_b = blen;
        for (size_t di = 0; di <= 64 && i + di < alen; di++) {
            for (size_t dj = 0; dj <= 64 && j + dj < blen; dj++) {
                if (strcmp(a[i + di], b[j + dj]) == 0) {
                    if (di + dj < best_a - i + best_b - j) {
                        best_a = i + di;
                        best_b = j + dj;
                    }
                    break;
                }
            }
        }

        size_t del_start = i, del_end = best_a;
        size_t ins_start = j, ins_end = best_b;

        /* Hunk range with context */
        size_t ha = del_start > ctx ? del_start - ctx : 0;
        size_t hb = ins_start > ctx ? ins_start - ctx : 0;
        size_t hea = del_end + ctx < alen ? del_end + ctx : alen;
        size_t heb = ins_end + ctx < blen ? ins_end + ctx : blen;

        O("@@ -%zu,%zu +%zu,%zu @@\n",
          ha + 1, hea - ha, hb + 1, heb - hb);

        /* Context before */
        for (size_t k = ha; k < del_start; k++)
            O(" %s\n", a[k]);

        /* Deleted lines */
        for (size_t k = del_start; k < del_end; k++)
            O("-%s\n", a[k]);

        /* Inserted lines */
        for (size_t k = ins_start; k < ins_end; k++)
            O("+%s\n", b[k]);

        /* Context after */
        for (size_t k = del_end; k < hea; k++)
            O(" %s\n", a[k]);

        i = hea;
        j = heb;
    }

    #undef O

    /* Restore newlines in copies (split_lines mutated them) */
    (void)acopy; (void)bcopy;

    free(a); free(b); free(acopy); free(bcopy);
    return out;

oom:
    free(a); free(b); free(acopy); free(bcopy); free(out);
    return NULL;
}

char *pe_diff(pe_t *pe, const char *path, size_t context) {
    pthread_mutex_lock(&pe->sched_lock);
    pe_file_t *f = ht_find(pe, path);
    pthread_mutex_unlock(&pe->sched_lock);
    if (!f || !f->original) return NULL;

    pthread_rwlock_rdlock(&f->lock);
    char *result = diff_unified(f->original, f->original_size,
                                 f->data, f->size, path, context);
    pthread_rwlock_unlock(&f->lock);
    return result;
}

/* ═══════════════════════════════════════════════════════════
 * Transactions
 * ═══════════════════════════════════════════════════════════ */

int pe_txn_begin(pe_t *pe) {
    pthread_mutex_lock(&pe->sched_lock);
    if (pe->txn_active) {
        pthread_mutex_unlock(&pe->sched_lock);
        return -1;  /* non-nesting */
    }
    pe->txn_active  = true;
    pe->txn_nedits  = 0;
    pthread_mutex_unlock(&pe->sched_lock);
    return 0;
}

size_t pe_txn_commit(pe_t *pe) {
    size_t nfailed = 0;

    pthread_mutex_lock(&pe->sched_lock);
    if (!pe->txn_active) {
        pthread_mutex_unlock(&pe->sched_lock);
        return 0;
    }

    /* Submit all queued edits */
    for (size_t i = 0; i < pe->txn_nedits; i++) {
        pe_edit_t *edit = pe->txn_edits[i];
        /* Resolve file if not already done */
        if (!edit->file) {
            pe_file_t *f = ht_find(pe, edit->path);
            if (!f) {
                /* File not in cache — can't submit; this shouldn't happen
                   since pe_edit_submit handles this, but if it does we
                   skip the edit */
                edit->error = strdup("file not cached");
                nfailed++;
                continue;
            }
            edit->file = f;
        }
        pe->pending++;
        if (edit->deps_remaining == 0)
            ready_push(pe, edit);
        pthread_cond_signal(&pe->sched_cond);
    }

    pe->txn_active  = false;
    pe->txn_nedits  = 0;
    pthread_mutex_unlock(&pe->sched_lock);

    /* Wait for all submitted edits */
    pe_flush(pe);
    return nfailed;
}

void pe_txn_rollback(pe_t *pe) {
    pthread_mutex_lock(&pe->sched_lock);
    if (!pe->txn_active) {
        pthread_mutex_unlock(&pe->sched_lock);
        return;
    }
    /* Free all queued edits */
    for (size_t i = 0; i < pe->txn_nedits; i++)
        pe_edit_free(pe->txn_edits[i]);
    pe->txn_nedits  = 0;
    pe->txn_active  = false;
    pthread_mutex_unlock(&pe->sched_lock);
}

bool pe_txn_active(pe_t *pe) {
    pthread_mutex_lock(&pe->sched_lock);
    bool a = pe->txn_active;
    pthread_mutex_unlock(&pe->sched_lock);
    return a;
}

/* ═══════════════════════════════════════════════════════════
 * Branches
 * ═══════════════════════════════════════════════════════════ */

/* Deep-copy a file entry */
static pe_file_t *file_deep_copy(pe_file_t *src) {
    pe_file_t *dst = calloc(1, sizeof(pe_file_t));
    if (!dst) return NULL;
    dst->path  = strdup(src->path);
    dst->size  = src->size;
    dst->data  = malloc(src->size + 1);
    if (dst->data) { memcpy(dst->data, src->data, src->size); dst->data[src->size] = '\0'; }
    dst->original = src->original ? malloc(src->original_size + 1) : NULL;
    if (dst->original) { memcpy(dst->original, src->original, src->original_size);
                         dst->original[dst->original_size] = '\0'; }
    dst->original_size = src->original_size;
    dst->nlines = src->nlines;
    dst->lines  = malloc(src->nlines * sizeof(size_t));
    if (dst->lines) memcpy(dst->lines, src->lines, src->nlines * sizeof(size_t));
    atomic_init(&dst->dirty, atomic_load(&src->dirty));
    pthread_rwlock_init(&dst->lock, NULL);
    return dst;
}

/* Deep-copy current hash table into a new branch */
static int branch_snapshot(pe_t *pe, struct pe_branch_s *br) {
    br->files_cap = pe->files_cap ? pe->files_cap : 64;
    br->files = calloc(br->files_cap, sizeof(pe_file_t *));
    if (!br->files) return -1;
    br->tombstone = calloc(1, sizeof(pe_file_t));
    if (!br->tombstone) { free(br->files); return -1; }

    for (size_t i = 0; i < pe->files_cap; i++) {
        pe_file_t *f = pe->files[i];
        if (!f || f == pe->tombstone) continue;
        pe_file_t *copy = file_deep_copy(f);
        if (!copy) continue;
        /* Insert into branch table */
        uint64_t h = ht_hash(copy->path);
        size_t idx = h % br->files_cap;
        while (br->files[idx] && br->files[idx] != br->tombstone)
            idx = (idx + 1) % br->files_cap;
        br->files[idx] = copy;
        br->files_count++;
    }
    return 0;
}

/* Free a branch's file table */
static void branch_free_files(struct pe_branch_s *br) {
    if (!br->files) return;
    for (size_t i = 0; i < br->files_cap; i++) {
        pe_file_t *f = br->files[i];
        if (!f || f == br->tombstone) continue;
        pthread_rwlock_destroy(&f->lock);
        free(f->path); free(f->data); free(f->original);
        free(f->lines);
        if (f->undo_stack) {
            for (size_t j = 0; j < f->undo_depth; j++)
                free(f->undo_stack[j].old_content);
        }
        free(f->undo_stack);
        free(f);
    }
    free(br->files);
    free(br->tombstone);
}

/* Find branch index by name, returns SIZE_MAX if not found */
static size_t branch_find(pe_t *pe, const char *name) {
    for (size_t i = 0; i < pe->nbranches; i++)
        if (strcmp(pe->branches[i].name, name) == 0) return i;
    return SIZE_MAX;
}

int pe_branch_create(pe_t *pe, const char *name) {
    pthread_mutex_lock(&pe->sched_lock);

    if (branch_find(pe, name) != SIZE_MAX) {
        pthread_mutex_unlock(&pe->sched_lock);
        return -1;
    }

    if (pe->nbranches >= pe->branches_cap) {
        size_t nc = pe->branches_cap ? pe->branches_cap * 2 : 4;
        void *np = realloc(pe->branches, nc * sizeof(*pe->branches));
        if (!np) { pthread_mutex_unlock(&pe->sched_lock); return -1; }
        pe->branches = np;
        pe->branches_cap = nc;
    }

    struct pe_branch_s *br = &pe->branches[pe->nbranches];
    memset(br, 0, sizeof(*br));
    br->name = strdup(name);
    if (!br->name) { pthread_mutex_unlock(&pe->sched_lock); return -1; }

    if (branch_snapshot(pe, br) != 0) {
        free(br->name);
        pthread_mutex_unlock(&pe->sched_lock);
        return -1;
    }

    pe->nbranches++;
    pthread_mutex_unlock(&pe->sched_lock);
    return 0;
}

int pe_branch_switch(pe_t *pe, const char *name) {
    pthread_mutex_lock(&pe->sched_lock);

    if (pe->current_branch) {
        size_t bi = branch_find(pe, pe->current_branch);
        if (bi != SIZE_MAX) {
            branch_free_files(&pe->branches[bi]);
            memset(&pe->branches[bi], 0, sizeof(pe->branches[bi]));
            pe->branches[bi].name = strdup(pe->current_branch);
            branch_snapshot(pe, &pe->branches[bi]);
        }
    } else {
        size_t bi = branch_find(pe, "main");
        if (bi == SIZE_MAX) {
            pthread_mutex_unlock(&pe->sched_lock);
            pe_branch_create(pe, "main");
            pthread_mutex_lock(&pe->sched_lock);
            bi = branch_find(pe, "main");
        }
        if (bi != SIZE_MAX) {
            branch_free_files(&pe->branches[bi]);
            memset(&pe->branches[bi], 0, sizeof(pe->branches[bi]));
            pe->branches[bi].name = strdup("main");
            branch_snapshot(pe, &pe->branches[bi]);
        }
    }

    /* Load target branch */
    size_t bi = branch_find(pe, name);
    if (bi == SIZE_MAX) {
        pthread_mutex_unlock(&pe->sched_lock);
        return -1;
    }

    /* Free current file table */
    for (size_t i = 0; i < pe->files_cap; i++) {
        pe_file_t *f = pe->files[i];
        if (!f || f == pe->tombstone) continue;
        pthread_rwlock_destroy(&f->lock);
        free(f->path); free(f->data); free(f->original);
        free(f->lines);
        if (f->undo_stack) {
            for (size_t j = 0; j < f->undo_depth; j++)
                free(f->undo_stack[j].old_content);
        }
        free(f->undo_stack);
        free(f);
    }
    free(pe->files);
    free(pe->tombstone);

    /* Swap in branch files */
    pe->files       = pe->branches[bi].files;
    pe->files_cap   = pe->branches[bi].files_cap;
    pe->files_count = pe->branches[bi].files_count;
    pe->tombstone   = pe->branches[bi].tombstone;

    /* Clear branch's pointers (ownership transferred) */
    pe->branches[bi].files     = NULL;
    pe->branches[bi].files_cap = 0;
    pe->branches[bi].files_count = 0;
    pe->branches[bi].tombstone = NULL;

    free(pe->current_branch);
    pe->current_branch = strdup(name);

    pthread_mutex_unlock(&pe->sched_lock);
    return 0;
}

static char *merge_file(const char *orig, size_t osz,
                        const char *ver_a, size_t asz,
                        const char *ver_b, size_t bsz,
                        int *had_conflicts) {
    /* If no original (newly created file), treat orig as empty */
    if (!orig) { orig = ""; osz = 0; }

    bool a_changed = (asz != osz || memcmp(orig, ver_a, osz < asz ? osz : asz) != 0);
    bool b_changed = (bsz != osz || memcmp(orig, ver_b, osz < bsz ? osz : bsz) != 0);

    if (!a_changed && !b_changed) {
        char *r = malloc(asz + 1);
        if (r) { memcpy(r, ver_a, asz); r[asz] = '\0'; }
        return r;
    }
    if (!b_changed) { char *r = malloc(asz + 1); if (r) { memcpy(r, ver_a, asz); r[asz] = '\0'; } return r; }
    if (!a_changed) { char *r = malloc(bsz + 1); if (r) { memcpy(r, ver_b, bsz); r[bsz] = '\0'; } return r; }

    /* Both changed — line-based 3-way merge */
    char *oc = malloc(osz + 1), *ac = malloc(asz + 1), *bc = malloc(bsz + 1);
    if (!oc || !ac || !bc) { free(oc); free(ac); free(bc); return NULL; }
    memcpy(oc, orig, osz); oc[osz] = '\0';
    memcpy(ac, ver_a, asz); ac[asz] = '\0';
    memcpy(bc, ver_b, bsz); bc[bsz] = '\0';

    size_t on, an, bn;
    char **ol = split_lines(oc, osz, &on);
    char **al = split_lines(ac, asz, &an);
    char **bl = split_lines(bc, bsz, &bn);
    if (!ol || !al || !bl) { free(ol); free(al); free(bl); free(oc); free(ac); free(bc); return NULL; }

    size_t cap = asz + bsz + 4096;
    char *out = malloc(cap);
    if (!out) { free(ol); free(al); free(bl); free(oc); free(ac); free(bc); return NULL; }
    size_t ol_off = 0;
    #define MO(...) do { \
        int _n = snprintf(out + ol_off, cap - ol_off, __VA_ARGS__); \
        if (_n < 0) _n = 0; \
        while (ol_off + (size_t)_n + 1 > cap) { \
            cap *= 2; char *_t = realloc(out, cap); if (!_t) goto merge_oom; out = _t; \
            _n = snprintf(out + ol_off, cap - ol_off, __VA_ARGS__); if (_n < 0) _n = 0; \
        } \
        ol_off += (size_t)_n; \
    } while(0)

    size_t oi = 0, ai = 0, bi = 0;
    while (oi < on || ai < an || bi < bn) {
        /* Skip lines identical in all 3 */
        while (oi < on && ai < an && bi < bn &&
               strcmp(ol[oi], al[ai]) == 0 && strcmp(ol[oi], bl[bi]) == 0) {
            MO("%s\n", ol[oi]);
            oi++; ai++; bi++;
        }
        if (oi >= on && ai >= an && bi >= bn) break;

        /* A changed but B didn't: take A */
        if (ai < an && bi < bn && oi < on &&
            strcmp(al[ai], ol[oi]) != 0 && strcmp(bl[bi], ol[oi]) == 0) {
            MO("%s\n", al[ai]);
            ai++; oi++; bi++;
            continue;
        }

        /* B changed but A didn't: take B */
        if (bi < bn && ai < an && oi < on &&
            strcmp(bl[bi], ol[oi]) != 0 && strcmp(al[ai], ol[oi]) == 0) {
            MO("%s\n", bl[bi]);
            bi++; oi++; ai++;
            continue;
        }

        /* Both changed same line — conflict */
        if (ai < an && bi < bn && oi < on &&
            strcmp(al[ai], ol[oi]) != 0 && strcmp(bl[bi], ol[oi]) != 0) {
            MO("<<<<<<< A\n");
            while (ai < an && (oi >= on || strcmp(al[ai], ol[oi]) != 0)) {
                MO("%s\n", al[ai]); ai++;
            }
            MO("=======\n");
            while (bi < bn && (oi >= on || strcmp(bl[bi], ol[oi]) != 0)) {
                MO("%s\n", bl[bi]); bi++;
            }
            MO(">>>>>>> B\n");
            *had_conflicts = 1;
            continue;
        }

        /* Fallback: advance to next sync point */
        if (ai < an) { MO("%s\n", al[ai]); ai++; }
        else if (bi < bn) { MO("%s\n", bl[bi]); bi++; }
        if (oi < on) oi++;
    }

    #undef MO

    free(ol); free(al); free(bl); free(oc); free(ac); free(bc);
    return out;

merge_oom:
    free(ol); free(al); free(bl); free(oc); free(ac); free(bc);
    free(out);
    return NULL;
}

int pe_branch_merge(pe_t *pe, const char *from, const char *to) {
    pthread_mutex_lock(&pe->sched_lock);

    size_t fi = branch_find(pe, from);
    size_t ti = branch_find(pe, to);
    if (fi == SIZE_MAX || ti == SIZE_MAX) {
        pthread_mutex_unlock(&pe->sched_lock);
        return -1;
    }

    int conflicts = 0;

    /* Iterate files in 'from' branch, merge into current */
    struct pe_branch_s *fb = &pe->branches[fi];
    if (!fb->files) { pthread_mutex_unlock(&pe->sched_lock); return 0; }
    for (size_t i = 0; i < fb->files_cap; i++) {
        pe_file_t *ff = fb->files[i];
        if (!ff || ff == fb->tombstone) continue;

        /* Find corresponding file in current cache */
        pe_file_t *tf = ht_find(pe, ff->path);

        if (!tf) {
            /* File exists in 'from' but not in current — copy it */
            pe_file_t *cp = file_deep_copy(ff);
            if (cp) {
                uint64_t h = ht_hash(cp->path);
                if (pe->files_cap == 0 || pe->files_count * 100 >= pe->files_cap * 70)
                    ht_grow(pe);
                size_t idx = h % pe->files_cap;
                while (pe->files[idx] && pe->files[idx] != pe->tombstone)
                    idx = (idx + 1) % pe->files_cap;
                pe->files[idx] = cp;
                pe->files_count++;
            }
            continue;
        }

        /* Both have the file — merge if both changed */
        bool f_changed = (!ff->original || ff->size != ff->original_size ||
                          memcmp(ff->data, ff->original, ff->size) != 0);
        bool t_changed = (!tf->original || tf->size != tf->original_size ||
                          memcmp(tf->data, tf->original, tf->size) != 0);

        if (!f_changed && !t_changed) continue;
        if (!f_changed) continue;  /* no new changes in 'from' */
        if (!t_changed) {
            /* Only 'from' changed — copy to current */
            pthread_rwlock_wrlock(&tf->lock);
            free(tf->data); free(tf->lines);
            if (tf->undo_stack) {
                for (size_t j = 0; j < tf->undo_depth; j++)
                    free(tf->undo_stack[j].old_content);
            }
            free(tf->undo_stack);
            tf->undo_stack = NULL; tf->undo_depth = 0; tf->undo_cap = 0;
            tf->size = ff->size;
            tf->data = malloc(ff->size + 1);
            if (tf->data) { memcpy(tf->data, ff->data, ff->size); tf->data[ff->size] = '\0'; }
            tf->nlines = ff->nlines;
            tf->lines = malloc(ff->nlines * sizeof(size_t));
            if (tf->lines) memcpy(tf->lines, ff->lines, ff->nlines * sizeof(size_t));
            atomic_store(&tf->dirty, 1);
            pthread_rwlock_unlock(&tf->lock);
            continue;
        }

        /* Both changed — 3-way merge */
        int fc = 0;
        char *merged = merge_file(tf->original, tf->original_size,
                                   ff->data, ff->size,
                                   tf->data, tf->size, &fc);
        if (!merged) continue;

        pthread_rwlock_wrlock(&tf->lock);
        free(tf->data); free(tf->lines);
        if (tf->undo_stack) {
            for (size_t j = 0; j < tf->undo_depth; j++)
                free(tf->undo_stack[j].old_content);
        }
        free(tf->undo_stack);
        tf->undo_stack = NULL; tf->undo_depth = 0; tf->undo_cap = 0;
        tf->size = strlen(merged);
        tf->data = merged;
        build_lines(tf->data, tf->size, &tf->lines, &tf->nlines);
        atomic_store(&tf->dirty, 1);
        pthread_rwlock_unlock(&tf->lock);

        if (fc) conflicts++;
    }

    pthread_mutex_unlock(&pe->sched_lock);
    return conflicts > 0 ? 1 : 0;
}

int pe_branch_delete(pe_t *pe, const char *name) {
    pthread_mutex_lock(&pe->sched_lock);

    if (pe->current_branch && strcmp(pe->current_branch, name) == 0) {
        pthread_mutex_unlock(&pe->sched_lock);
        return -1;
    }

    size_t bi = branch_find(pe, name);
    if (bi == SIZE_MAX) {
        /* Check for "main" default */
        if (!pe->current_branch && strcmp(name, "main") == 0) {
            pthread_mutex_unlock(&pe->sched_lock);
            return -1;  /* can't delete current */
        }
        pthread_mutex_unlock(&pe->sched_lock);
        return -1;
    }

    branch_free_files(&pe->branches[bi]);
    free(pe->branches[bi].name);
    /* Compact array */
    for (size_t i = bi; i + 1 < pe->nbranches; i++)
        pe->branches[i] = pe->branches[i + 1];
    pe->nbranches--;

    pthread_mutex_unlock(&pe->sched_lock);
    return 0;
}

size_t pe_branch_list(pe_t *pe, char ***names) {
    pthread_mutex_lock(&pe->sched_lock);
    size_t n = pe->nbranches;
    if (!pe->current_branch) n++;  /* implicit "main" */
    *names = malloc(n * sizeof(char *));
    if (*names) {
        size_t j = 0;
        for (size_t i = 0; i < pe->nbranches; i++)
            (*names)[j++] = strdup(pe->branches[i].name);
        if (!pe->current_branch)
            (*names)[j++] = strdup("main");
    }
    pthread_mutex_unlock(&pe->sched_lock);
    return n;
}

const char *pe_branch_current(pe_t *pe) {
    return pe->current_branch ? pe->current_branch : "main";
}
