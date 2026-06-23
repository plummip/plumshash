/*
 * pe.h — Project Editor: single-header C library + daemon
 *
 * Zero-dependency (libc + pthreads).  Header-only — define PE_IMPL
 * in ONE translation unit to get the implementation.  Define
 * PE_DAEMON to also include the Unix-socket daemon main().
 *
 *   #define PE_IMPL
 *   #include "pe.h"        // library
 *
 *   #define PE_DAEMON
 *   #include "pe.h"        // daemon (implies PE_IMPL)
 *   // gcc -std=c11 -O2 -o mydaemon mydaemon.c -pthread
 *
 * Features:
 *   - In-memory file cache + line index (O(1) line→offset)
 *   - Parallel edit workers (thread pool)
 *   - Dependency DAG for ordered edits
 *   - Surgical line/column positions (1-indexed)
 *   - Undo stack per file
 *   - Unified diff (line-hash based, O(N))
 *   - Atomic transactions (BEGIN/COMMIT/ROLLBACK)
 *   - Named branches with 3-way merge
 *   - Unix-socket daemon protocol
 *
 * License: MIT
 * Author:  coder
 */
#ifndef PE_H
#define PE_H

#include <stddef.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <pthread.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ═══════════════════════════════════════════════════════════
 * Public Types
 * ═══════════════════════════════════════════════════════════ */

typedef struct pe_s pe_t;

typedef enum { PE_OP_INSERT, PE_OP_DELETE, PE_OP_REPLACE } pe_op_t;

typedef enum {
    PE_EDIT_PENDING, PE_EDIT_READY, PE_EDIT_RUNNING,
    PE_EDIT_DONE, PE_EDIT_FAILED
} pe_edit_state_t;

typedef struct { size_t line, col; } pe_pos_t;  /* 1-indexed */
typedef struct pe_edit_s pe_edit_t;

/* ═══════════════════════════════════════════════════════════
 * Library API
 * ═══════════════════════════════════════════════════════════ */

pe_t          *pe_create(const char *root, size_t nworkers);
void           pe_destroy(pe_t *pe);
int            pe_cache_file(pe_t *pe, const char *path);
pe_edit_t     *pe_edit_create(pe_t *pe, const char *path, pe_op_t op,
                              pe_pos_t start, pe_pos_t end,
                              const char *content, size_t len);
int            pe_edit_depend(pe_edit_t *edit, pe_edit_t *dep);
int            pe_edit_submit(pe_t *pe, pe_edit_t *edit);
size_t         pe_flush(pe_t *pe);
int            pe_sync(pe_t *pe);
pe_edit_state_t pe_edit_state(pe_edit_t *edit);
const char    *pe_edit_error(pe_edit_t *edit);
void           pe_edit_free(pe_edit_t *edit);
const char    *pe_file_data(pe_t *pe, const char *path, size_t *size);
size_t         pe_file_lines(pe_t *pe, const char *path);

/* Thread-safe file access: hold rdlock while reading data.
 * pe_file_data is safe when (a) no concurrent edits, or (b) rdlock held. */
int            pe_file_rdlock(pe_t *pe, const char *path);
void           pe_file_rdunlock(pe_t *pe, const char *path);
int            pe_file_wrlock(pe_t *pe, const char *path);
void           pe_file_wrunlock(pe_t *pe, const char *path);
bool           pe_file_exists(pe_t *pe, const char *path);     /* cached? */
size_t         pe_file_size(pe_t *pe, const char *path);       /* byte size */

/* Diff (line-hash based, O(N)) */
char          *pe_diff(pe_t *pe, const char *path, size_t context);

/* Undo */
int            pe_undo(pe_t *pe, const char *path);
size_t         pe_undo_depth(pe_t *pe, const char *path);

/* Transactions */
int            pe_txn_begin(pe_t *pe);
size_t         pe_txn_commit(pe_t *pe);
void           pe_txn_rollback(pe_t *pe);
bool           pe_txn_active(pe_t *pe);

/* Branches */
int            pe_branch_create(pe_t *pe, const char *name);
int            pe_branch_switch(pe_t *pe, const char *name);
int            pe_branch_merge(pe_t *pe, const char *from, const char *to);
int            pe_branch_delete(pe_t *pe, const char *name);
size_t         pe_branch_list(pe_t *pe, char ***names);
const char    *pe_branch_current(pe_t *pe);

#ifdef __cplusplus
}
#endif

#endif /* PE_H */

/* ═══════════════════════════════════════════════════════════
 * Implementation
 * ═══════════════════════════════════════════════════════════ */
#ifdef PE_IMPL

#define _GNU_SOURCE
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdint.h>
#include <stdarg.h>
#include <unistd.h>
#include <errno.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>

/* ═══════════════════════════════════════════════════════════
 * Internal Types
 * ═══════════════════════════════════════════════════════════ */

typedef struct {
    char       *path, *data, *original;
    uint64_t    path_hash;        /* cached FNV-1a */
    size_t      size, original_size;
    size_t     *lines, nlines;
    _Atomic int dirty;

    struct pe_undo_s { pe_op_t op; size_t so, eo; char *old; size_t olen; }
        *undo_stack;
    size_t      undo_depth, undo_cap;

    pthread_rwlock_t lock;
} pe_file_t;

struct pe_edit_s {
    pe_t *pe; char *path; pe_file_t *file;
    pe_op_t op; pe_pos_t start, end;
    char *content; size_t content_len;
    size_t ndeps, deps_remaining;
    pe_edit_t **rdeps; size_t nrdeps, nrdeps_cap;
    _Atomic pe_edit_state_t state;
    char *error;
    pe_edit_t *qnext, *anext;
};

typedef struct { char *name; pe_file_t **files; size_t cap, count; pe_file_t *tomb; } pe_branch_t;

struct pe_s {
    char *root; size_t root_len;
    pe_file_t **files, *tombstone;
    size_t files_cap, files_count;
    pthread_t *workers; size_t nworkers; bool running;
    pe_edit_t *ready_head, *ready_tail;
    pthread_mutex_t sched_lock;
    pthread_cond_t sched_cond, done_cond;
    size_t pending, nfailed;
    bool txn_active; pe_edit_t **txn_edits; size_t txn_nedits, txn_cap;
    pe_branch_t *branches; size_t nbranches, branches_cap;
    char *current_branch;
    pe_edit_t *all_edits;
};

/* ═══════════════════════════════════════════════════════════
 * Fast hash: FNV-1a 64-bit (inline)
 * ═══════════════════════════════════════════════════════════ */

static inline uint64_t pe_hash(const char *restrict s) {
    uint64_t h = 14695981039346656037ULL;
    for (; *s; s++) { h ^= (uint64_t)(unsigned char)*s; h *= 1099511628211ULL; }
    return h;
}

/* ═══════════════════════════════════════════════════════════
 * Hash Table (open-addressing, linear probing)
 * ═══════════════════════════════════════════════════════════ */

#define HT_MIN_CAP 64
#define HT_LOAD_PCT 70

static int ht_grow(pe_t *pe) {
    size_t oc = pe->files_cap; pe_file_t **old = pe->files;
    size_t nc = oc ? oc * 2 : HT_MIN_CAP;
    pe_file_t **n = calloc(nc, sizeof(pe_file_t *));
    if (!n) return -1;
    for (size_t i = 0; i < oc; i++) {
        pe_file_t *e = old[i];
        if (!e || e == pe->tombstone) continue;
        size_t idx = e->path_hash % nc;
        while (n[idx]) idx = (idx + 1) % nc;
        n[idx] = e;
    }
    free(old); pe->files = n; pe->files_cap = nc;
    return 0;
}

static pe_file_t *ht_find(pe_t *pe, const char *restrict path) {
    if (!pe->files_cap) return NULL;
    uint64_t h = pe_hash(path);
    size_t idx = h % pe->files_cap, orig = idx;
    do {
        pe_file_t *e = pe->files[idx];
        if (!e) return NULL;
        if (e != pe->tombstone && e->path_hash == h &&
            strcmp(e->path, path) == 0) return e;
        idx = (idx + 1) % pe->files_cap;
    } while (idx != orig);
    return NULL;
}

static int ht_insert(pe_t *pe, pe_file_t *entry) {
    if (pe->files_count * 100 >= pe->files_cap * HT_LOAD_PCT)
        if (ht_grow(pe) != 0) return -1;
    uint64_t h = entry->path_hash;
    size_t idx = h % pe->files_cap;
    while (pe->files[idx] && pe->files[idx] != pe->tombstone) {
        if (pe->files[idx]->path_hash == h &&
            strcmp(pe->files[idx]->path, entry->path) == 0) return -1;
        idx = (idx + 1) % pe->files_cap;
    }
    pe->files[idx] = entry; pe->files_count++;
    return 0;
}

/* ═══════════════════════════════════════════════════════════
 * Path helpers
 * ═══════════════════════════════════════════════════════════ */

static char *pe_mkfull(pe_t *pe, const char *rel, size_t *len) {
    size_t rl = strlen(rel), total = pe->root_len + 1 + rl;
    char *f = malloc(total + 1);
    if (!f) return NULL;
    memcpy(f, pe->root, pe->root_len); f[pe->root_len] = '/';
    memcpy(f + pe->root_len + 1, rel, rl); f[total] = '\0';
    if (len) *len = total;
    return f;
}

/* ═══════════════════════════════════════════════════════════
 * File I/O: mmap for performance (fallback to read)
 * ═══════════════════════════════════════════════════════════ */

static int file_load(pe_t *pe, const char *rel, char **data, size_t *size) {
    size_t flen; char *full = pe_mkfull(pe, rel, &flen);
    if (!full) return -1;
    int fd = open(full, O_RDONLY);
    free(full);
    if (fd < 0) return -1;

    struct stat st;
    if (fstat(fd, &st) < 0) { close(fd); return -1; }
    size_t sz = (size_t)st.st_size;

    /* Use mmap for files > 4KB, read otherwise */
    char *buf;
    if (sz >= 4096) {
        buf = mmap(NULL, sz, PROT_READ, MAP_PRIVATE, fd, 0);
        if (buf == MAP_FAILED) { close(fd); return -1; }
        char *cpy = malloc(sz + 1);
        if (cpy) { memcpy(cpy, buf, sz); cpy[sz] = '\0'; }
        munmap(buf, sz);
        close(fd);
        if (!cpy) return -1;
        *data = cpy; *size = sz;
        return 0;
    }

    buf = malloc(sz + 1);
    if (!buf) { close(fd); return -1; }
    size_t n = 0;
    while (n < sz) {
        ssize_t r = read(fd, buf + n, sz - n);
        if (r <= 0) break;
        n += (size_t)r;
    }
    close(fd);
    if (n != sz) { free(buf); return -1; }
    buf[sz] = '\0';
    *data = buf; *size = sz;
    return 0;
}

/* ═══════════════════════════════════════════════════════════
 * Line index builder
 * ═══════════════════════════════════════════════════════════ */

static int build_lines(char *restrict data, size_t size,
                       size_t **lines, size_t *nlines) {
    size_t nl = 1;
    for (size_t i = 0; i < size; i++) if (data[i] == '\n') nl++;
    if (size > 0 && data[size - 1] == '\n') nl--;
    if (nl == 0) nl = 1;

    size_t *l = malloc(nl * sizeof(size_t));
    if (!l) return -1;
    l[0] = 0; size_t li = 1;
    for (size_t i = 0; i < size; i++)
        if (data[i] == '\n' && li < nl) l[li++] = i + 1;
    *lines = l; *nlines = nl;
    return 0;
}

/* ═══════════════════════════════════════════════════════════
 * Position resolver: O(1) via line index
 * ═══════════════════════════════════════════════════════════ */

static size_t resolve_pos(pe_file_t *restrict f, pe_pos_t pos) {
    if (pos.line == 0) return 0;
    if (pos.line == SIZE_MAX) return f->size;
    if (pos.line > f->nlines) return SIZE_MAX;
    size_t ls = f->lines[pos.line - 1], ll;
    if (pos.line < f->nlines) {
        ll = f->lines[pos.line] - ls;
        if (ll > 0 && f->data[f->lines[pos.line] - 1] == '\n') ll--;
    } else {
        ll = f->size - ls;
        if (ll > 0 && f->data[f->size - 1] == '\n') ll--;
    }
    if (pos.col == 0) return ls;
    if (pos.col > ll + 1) return SIZE_MAX;
    return ls + pos.col - 1;
}

/* ═══════════════════════════════════════════════════════════
 * Edit applicators (modify in-memory buffer)
 * ═══════════════════════════════════════════════════════════ */

static int apply_insert(pe_file_t *f, size_t off, const char *content, size_t len) {
    if (off > f->size) return -1;
    char *nd = realloc(f->data, f->size + len + 1);
    if (!nd) return -1;
    f->data = nd;
    memmove(f->data + off + len, f->data + off, f->size - off);
    memcpy(f->data + off, content, len);
    f->size += len; f->data[f->size] = '\0';
    atomic_store(&f->dirty, 1);
    free(f->lines);
    return build_lines(f->data, f->size, &f->lines, &f->nlines);
}

static int apply_delete(pe_file_t *f, size_t start, size_t end) {
    if (start > f->size || end > f->size || start > end) return -1;
    size_t len = end - start;
    if (!len) return 0;
    memmove(f->data + start, f->data + end, f->size - end);
    f->size -= len; f->data[f->size] = '\0';
    atomic_store(&f->dirty, 1);
    free(f->lines);
    return build_lines(f->data, f->size, &f->lines, &f->nlines);
}

static int apply_replace(pe_file_t *f, size_t s, size_t e, const char *c, size_t cl) {
    if (s > f->size || e > f->size || s > e) return -1;
    size_t ol = e - s;
    ssize_t delta = (ssize_t)cl - (ssize_t)ol;
    if (delta > 0) {
        char *nd = realloc(f->data, f->size + (size_t)delta + 1);
        if (!nd) return -1;
        f->data = nd;
    }
    if (delta != 0)
        memmove(f->data + s + cl, f->data + e, f->size - e);
    memcpy(f->data + s, c, cl);
    f->size = f->size - ol + cl; f->data[f->size] = '\0';
    atomic_store(&f->dirty, 1);
    free(f->lines);
    return build_lines(f->data, f->size, &f->lines, &f->nlines);
}

/* ═══════════════════════════════════════════════════════════
 * Scheduler: ready queue, workers, DAG
 * ═══════════════════════════════════════════════════════════ */

static void ready_push(pe_t *pe, pe_edit_t *edit) {
    edit->state = PE_EDIT_READY; edit->qnext = NULL;
    if (pe->ready_tail) pe->ready_tail->qnext = edit;
    else pe->ready_head = edit;
    pe->ready_tail = edit;
}

static void complete_edit(pe_t *pe, pe_edit_t *edit) {
    edit->state = edit->error ? PE_EDIT_FAILED : PE_EDIT_DONE;
    if (edit->error) pe->nfailed++;
    pe->pending--;
    for (size_t i = 0; i < edit->nrdeps; i++) {
        pe_edit_t *rd = edit->rdeps[i];
        if (!rd->deps_remaining) continue;
        if (!--rd->deps_remaining && rd->file) ready_push(pe, rd);
    }
    if (!pe->pending) pthread_cond_broadcast(&pe->done_cond);
}

static void apply_edit(pe_edit_t *edit) {
    pe_file_t *f = edit->file;
    pthread_rwlock_wrlock(&f->lock);
    size_t os = resolve_pos(f, edit->start);
    if (os == SIZE_MAX) { edit->error = strdup("bad start"); goto out; }
    size_t oe = 0;
    if (edit->op != PE_OP_INSERT) {
        oe = (!edit->end.line && !edit->end.col) ? f->size : resolve_pos(f, edit->end);
        if (oe == SIZE_MAX) { edit->error = strdup("bad end"); goto out; }
    }

    char *old = NULL; size_t oldlen = 0;
    if (edit->op != PE_OP_INSERT) {
        oldlen = oe - os;
        if (oldlen) { old = malloc(oldlen); if (old) memcpy(old, f->data + os, oldlen); }
    }

    int rc = 0;
    switch (edit->op) {
    case PE_OP_INSERT:  rc = apply_insert(f, os, edit->content, edit->content_len); break;
    case PE_OP_DELETE:  rc = apply_delete(f, os, oe); break;
    case PE_OP_REPLACE: rc = apply_replace(f, os, oe, edit->content, edit->content_len); break;
    }

    if (rc) { free(old); edit->error = strdup("apply fail"); }
    else if (f->undo_depth < f->undo_cap || (
        f->undo_cap = f->undo_cap ? f->undo_cap * 2 : 16,
        (f->undo_stack = realloc(f->undo_stack, f->undo_cap * sizeof(*f->undo_stack))) )) {
        struct pe_undo_s *u = &f->undo_stack[f->undo_depth++];
        u->op = edit->op; u->so = os;
        u->eo = (edit->op == PE_OP_INSERT) ? os + edit->content_len : oe;
        u->old = old; u->olen = oldlen; old = NULL;
    }
    free(old);
out:
    pthread_rwlock_unlock(&f->lock);
}

static void *worker_main(void *arg) {
    pe_t *pe = (pe_t *)arg;
    pthread_mutex_lock(&pe->sched_lock);
    for (;;) {
        while (!pe->ready_head && pe->running)
            pthread_cond_wait(&pe->sched_cond, &pe->sched_lock);
        if (!pe->running && !pe->ready_head) break;
        pe_edit_t *e = pe->ready_head;
        pe->ready_head = e->qnext;
        if (!pe->ready_head) pe->ready_tail = NULL;
        e->state = PE_EDIT_RUNNING;
        pthread_mutex_unlock(&pe->sched_lock);
        apply_edit(e);
        pthread_mutex_lock(&pe->sched_lock);
        complete_edit(pe, e);
    }
    pthread_mutex_unlock(&pe->sched_lock);
    return NULL;
}

/* ═══════════════════════════════════════════════════════════
 * Core API
 * ═══════════════════════════════════════════════════════════ */

pe_t *pe_create(const char *root, size_t nworkers) {
    pe_t *pe = calloc(1, sizeof(pe_t));
    if (!pe) return NULL;
    size_t rl = strlen(root); while (rl && root[rl-1] == '/') rl--;
    if (!(pe->root = malloc(rl + 1))) { free(pe); return NULL; }
    memcpy(pe->root, root, rl); pe->root[rl] = '\0'; pe->root_len = rl;
    if (!(pe->tombstone = calloc(1, sizeof(pe_file_t)))) { free(pe->root); free(pe); return NULL; }
    pe->nworkers = nworkers ? nworkers : (size_t)(sysconf(_SC_NPROCESSORS_ONLN) > 0 ? sysconf(_SC_NPROCESSORS_ONLN) : 4);
    if (pthread_mutex_init(&pe->sched_lock, NULL) ||
        pthread_cond_init(&pe->sched_cond, NULL) || pthread_cond_init(&pe->done_cond, NULL))
        { free(pe->tombstone); free(pe->root); free(pe); return NULL; }
    if (!(pe->workers = malloc(pe->nworkers * sizeof(pthread_t)))) goto fail;
    pe->running = true;
    size_t i; for (i = 0; i < pe->nworkers; i++)
        if (pthread_create(&pe->workers[i], NULL, worker_main, pe)) break;
    if (i < pe->nworkers) { pe->running = false; pthread_cond_broadcast(&pe->sched_cond);
        for (size_t j = 0; j < i; j++) pthread_join(pe->workers[j], NULL);
        free(pe->workers); goto fail; }
    return pe;
fail:
    pthread_cond_destroy(&pe->done_cond); pthread_cond_destroy(&pe->sched_cond);
    pthread_mutex_destroy(&pe->sched_lock);
    free(pe->tombstone); free(pe->root); free(pe); return NULL;
}

void pe_destroy(pe_t *pe) {
    if (!pe) return;
    pe_flush(pe);
    pthread_mutex_lock(&pe->sched_lock); pe->running = false;
    pthread_cond_broadcast(&pe->sched_cond); pthread_mutex_unlock(&pe->sched_lock);
    for (size_t i = 0; i < pe->nworkers; i++) pthread_join(pe->workers[i], NULL);
    free(pe->workers);
    pe_edit_t *e = pe->all_edits;
    while (e) { pe_edit_t *nx = e->anext; free(e->path); free(e->content); free(e->rdeps); free(e->error); free(e); e = nx; }
    for (size_t i = 0; i < pe->files_cap; i++) {
        pe_file_t *f = pe->files[i]; if (!f || f == pe->tombstone) continue;
        pthread_rwlock_destroy(&f->lock);
        free(f->path); free(f->data); free(f->original); free(f->lines);
        if (f->undo_stack) { for (size_t j = 0; j < f->undo_depth; j++) free(f->undo_stack[j].old); }
        free(f->undo_stack); free(f);
    }
    free(pe->files); free(pe->tombstone); free(pe->txn_edits);
    for (size_t i = 0; i < pe->nbranches; i++) {
        free(pe->branches[i].name);
        if (pe->branches[i].files) {
            for (size_t j = 0; j < pe->branches[i].cap; j++) {
                pe_file_t *f = pe->branches[i].files[j];
                if (!f || f == pe->branches[i].tomb) continue;
                pthread_rwlock_destroy(&f->lock);
                free(f->path); free(f->data); free(f->original); free(f->lines);
                if (f->undo_stack) { for (size_t k = 0; k < f->undo_depth; k++) free(f->undo_stack[k].old); }
                free(f->undo_stack); free(f);
            }
            free(pe->branches[i].files); free(pe->branches[i].tomb);
        }
    }
    free(pe->branches); free(pe->current_branch);
    pthread_cond_destroy(&pe->done_cond); pthread_cond_destroy(&pe->sched_cond);
    pthread_mutex_destroy(&pe->sched_lock); free(pe->root); free(pe);
}

int pe_cache_file(pe_t *pe, const char *path) {
    pthread_mutex_lock(&pe->sched_lock); pe_file_t *f = ht_find(pe, path); pthread_mutex_unlock(&pe->sched_lock);
    if (f) return 0;
    f = calloc(1, sizeof(pe_file_t)); if (!f) return -1;
    if (!(f->path = strdup(path))) { free(f); return -1; }
    f->path_hash = pe_hash(f->path);
    if (file_load(pe, path, &f->data, &f->size)) { free(f->path); free(f); return -1; }
    if (build_lines(f->data, f->size, &f->lines, &f->nlines)) { free(f->data); free(f->path); free(f); return -1; }
    if ((f->original = malloc(f->size + 1))) { memcpy(f->original, f->data, f->size); f->original[f->size] = '\0'; f->original_size = f->size; }
    pthread_rwlock_init(&f->lock, NULL); atomic_init(&f->dirty, 0);
    pthread_mutex_lock(&pe->sched_lock);
    if (ht_insert(pe, f)) { pthread_rwlock_destroy(&f->lock); free(f->lines); free(f->data); free(f->original); free(f->path); free(f); f = ht_find(pe, path); if (!f) { pthread_mutex_unlock(&pe->sched_lock); return -1; } }
    pthread_mutex_unlock(&pe->sched_lock);
    return 0;
}

pe_edit_t *pe_edit_create(pe_t *pe, const char *path, pe_op_t op, pe_pos_t s, pe_pos_t e, const char *c, size_t cl) {
    pe_edit_t *ed = calloc(1, sizeof(pe_edit_t)); if (!ed) return NULL;
    ed->pe = pe; ed->path = strdup(path); if (!ed->path) { free(ed); return NULL; }
    ed->op = op; ed->start = s; ed->end = e; ed->state = PE_EDIT_PENDING;
    if (c && cl) { ed->content = malloc(cl + 1); if (!ed->content) { free(ed->path); free(ed); return NULL; } memcpy(ed->content, c, cl); ed->content[cl] = '\0'; ed->content_len = cl; }
    pthread_mutex_lock(&pe->sched_lock); ed->anext = pe->all_edits; pe->all_edits = ed; pthread_mutex_unlock(&pe->sched_lock);
    return ed;
}

int pe_edit_depend(pe_edit_t *ed, pe_edit_t *dep) {
    pe_t *pe = ed->pe;
    pthread_mutex_lock(&pe->sched_lock);
    if (dep->nrdeps >= dep->nrdeps_cap) {
        size_t nc = dep->nrdeps_cap ? dep->nrdeps_cap * 2 : 4;
        pe_edit_t **nr = realloc(dep->rdeps, nc * sizeof(pe_edit_t *));
        if (!nr) { pthread_mutex_unlock(&pe->sched_lock); return -1; }
        dep->rdeps = nr; dep->nrdeps_cap = nc;
    }
    dep->rdeps[dep->nrdeps++] = ed; ed->ndeps++; ed->deps_remaining++;
    pe_edit_state_t ds = atomic_load(&dep->state);
    if ((ds == PE_EDIT_DONE || ds == PE_EDIT_FAILED) && !--ed->deps_remaining && ed->file)
        ready_push(pe, ed);
    pthread_mutex_unlock(&pe->sched_lock);
    return 0;
}

int pe_edit_submit(pe_t *pe, pe_edit_t *ed) {
    pthread_mutex_lock(&pe->sched_lock); pe_file_t *f = ht_find(pe, ed->path); pthread_mutex_unlock(&pe->sched_lock);
    if (!f) {
        f = calloc(1, sizeof(pe_file_t)); if (!f) return -1;
        if (!(f->path = strdup(ed->path))) { free(f); return -1; }
        f->path_hash = pe_hash(f->path);
        if (file_load(pe, ed->path, &f->data, &f->size)) { free(f->path); free(f); return -1; }
        if (build_lines(f->data, f->size, &f->lines, &f->nlines)) { free(f->data); free(f->path); free(f); return -1; }
        if ((f->original = malloc(f->size + 1))) { memcpy(f->original, f->data, f->size); f->original[f->size] = '\0'; f->original_size = f->size; }
        pthread_rwlock_init(&f->lock, NULL); atomic_init(&f->dirty, 0);
        pthread_mutex_lock(&pe->sched_lock);
        if (ht_insert(pe, f)) { pthread_rwlock_destroy(&f->lock); free(f->lines); free(f->data); free(f->original); free(f->path); free(f); f = ht_find(pe, ed->path); if (!f) { pthread_mutex_unlock(&pe->sched_lock); return -1; } }
        pthread_mutex_unlock(&pe->sched_lock);
    }
    ed->file = f;
    pthread_mutex_lock(&pe->sched_lock);
    if (pe->txn_active) {
        if (pe->txn_nedits >= pe->txn_cap) { size_t nc = pe->txn_cap ? pe->txn_cap * 2 : 16; pe_edit_t **ne = realloc(pe->txn_edits, nc * sizeof(pe_edit_t *)); if (!ne) { pthread_mutex_unlock(&pe->sched_lock); return -1; } pe->txn_edits = ne; pe->txn_cap = nc; }
        pe->txn_edits[pe->txn_nedits++] = ed;
    } else {
        pe->pending++;
        if (!ed->deps_remaining) ready_push(pe, ed);
        pthread_cond_signal(&pe->sched_cond);
    }
    pthread_mutex_unlock(&pe->sched_lock);
    return 0;
}

size_t pe_flush(pe_t *pe) {
    pthread_mutex_lock(&pe->sched_lock);
    while (pe->pending) pthread_cond_wait(&pe->done_cond, &pe->sched_lock);
    size_t nf = pe->nfailed; pthread_mutex_unlock(&pe->sched_lock);
    return nf;
}

int pe_sync(pe_t *pe) {
    int errs = 0;
    for (size_t i = 0; i < pe->files_cap; i++) {
        pe_file_t *f = pe->files[i]; if (!f || f == pe->tombstone) continue;
        if (!atomic_exchange(&f->dirty, 0)) continue;
        pthread_rwlock_rdlock(&f->lock);
        size_t flen; char *full = pe_mkfull(pe, f->path, &flen);
        if (!full) { pthread_rwlock_unlock(&f->lock); errs++; continue; }
        /* Snapshot data under lock, then release for I/O */
        size_t sz = f->size;
        char *snap = malloc(sz);
        if (!snap) { free(full); pthread_rwlock_unlock(&f->lock); errs++; continue; }
        memcpy(snap, f->data, sz);
        pthread_rwlock_unlock(&f->lock);
        size_t tmplen = flen + 5;
        char *tmp = malloc(tmplen);
        if (!tmp) { free(snap); free(full); errs++; continue; }
        snprintf(tmp, tmplen, "%s.tmp", full);
        FILE *fp = fopen(tmp, "wb");
        if (!fp) { free(tmp); free(snap); free(full); errs++; continue; }
        size_t w = fwrite(snap, 1, sz, fp); int fe = ferror(fp); fclose(fp);
        if (w != sz || fe) { unlink(tmp); free(tmp); free(snap); free(full); errs++; continue; }
        if (rename(tmp, full)) { unlink(tmp); free(tmp); free(snap); free(full); errs++; continue; }
        free(tmp); free(full);
        pthread_rwlock_wrlock(&f->lock);
        free(f->original); f->original = snap; f->original_size = sz;
        pthread_rwlock_unlock(&f->lock);
    }
    return errs ? -1 : 0;
}

pe_edit_state_t pe_edit_state(pe_edit_t *e) { return atomic_load(&e->state); }
const char *pe_edit_error(pe_edit_t *e) { return e->error ? e->error : "no error"; }
const char *pe_file_data(pe_t *pe, const char *p, size_t *sz) { pe_file_t *f; pthread_mutex_lock(&pe->sched_lock); f = ht_find(pe, p); pthread_mutex_unlock(&pe->sched_lock); if (!f) { if (sz) *sz = 0; return NULL; } if (sz) *sz = f->size; return f->data; }
size_t pe_file_lines(pe_t *pe, const char *p) { pthread_mutex_lock(&pe->sched_lock); pe_file_t *f = ht_find(pe, p); pthread_mutex_unlock(&pe->sched_lock); return f ? f->nlines : 0; }

int pe_file_rdlock(pe_t *pe, const char *p) { pthread_mutex_lock(&pe->sched_lock); pe_file_t *f = ht_find(pe, p); pthread_mutex_unlock(&pe->sched_lock); if (!f) return -1; pthread_rwlock_rdlock(&f->lock); return 0; }
void pe_file_rdunlock(pe_t *pe, const char *p) { pthread_mutex_lock(&pe->sched_lock); pe_file_t *f = ht_find(pe, p); pthread_mutex_unlock(&pe->sched_lock); if (f) pthread_rwlock_unlock(&f->lock); }
int pe_file_wrlock(pe_t *pe, const char *p) { pthread_mutex_lock(&pe->sched_lock); pe_file_t *f = ht_find(pe, p); pthread_mutex_unlock(&pe->sched_lock); if (!f) return -1; pthread_rwlock_wrlock(&f->lock); return 0; }
void pe_file_wrunlock(pe_t *pe, const char *p) { pthread_mutex_lock(&pe->sched_lock); pe_file_t *f = ht_find(pe, p); pthread_mutex_unlock(&pe->sched_lock); if (f) pthread_rwlock_unlock(&f->lock); }
bool pe_file_exists(pe_t *pe, const char *p) { pthread_mutex_lock(&pe->sched_lock); pe_file_t *f = ht_find(pe, p); pthread_mutex_unlock(&pe->sched_lock); return f != NULL; }
size_t pe_file_size(pe_t *pe, const char *p) { pthread_mutex_lock(&pe->sched_lock); pe_file_t *f = ht_find(pe, p); pthread_mutex_unlock(&pe->sched_lock); return f ? f->size : 0; }

void pe_edit_free(pe_edit_t *ed) {
    if (!ed) return; pe_t *pe = ed->pe;
    pthread_mutex_lock(&pe->sched_lock);
    if (pe->all_edits == ed) pe->all_edits = ed->anext;
    else for (pe_edit_t *e = pe->all_edits; e; e = e->anext) if (e->anext == ed) { e->anext = ed->anext; break; }
    pthread_mutex_unlock(&pe->sched_lock);
    free(ed->path); free(ed->content); free(ed->rdeps); free(ed->error); free(ed);
}

/* ═══════════════════════════════════════════════════════════
 * Undo
 * ═══════════════════════════════════════════════════════════ */

int pe_undo(pe_t *pe, const char *path) {
    pthread_mutex_lock(&pe->sched_lock); pe_file_t *f = ht_find(pe, path); pthread_mutex_unlock(&pe->sched_lock);
    if (!f) return -1;
    pthread_rwlock_wrlock(&f->lock);
    if (!f->undo_depth) { pthread_rwlock_unlock(&f->lock); return -1; }
    struct pe_undo_s *u = &f->undo_stack[--f->undo_depth];
    int rc = 0;
    switch (u->op) {
    case PE_OP_INSERT:  rc = apply_delete(f, u->so, u->eo); break;
    case PE_OP_DELETE:  rc = apply_insert(f, u->so, u->old, u->olen); break;
    case PE_OP_REPLACE: rc = apply_delete(f, u->so, u->eo) ? -1 : apply_insert(f, u->so, u->old, u->olen); break;
    }
    free(u->old); u->old = NULL;
    pthread_rwlock_unlock(&f->lock);
    return rc;
}

size_t pe_undo_depth(pe_t *pe, const char *p) {
    pthread_mutex_lock(&pe->sched_lock); pe_file_t *f = ht_find(pe, p); pthread_mutex_unlock(&pe->sched_lock);
    return f ? f->undo_depth : 0;
}

/* ═══════════════════════════════════════════════════════════
 * Diff: line-hash based, O(N)
 * ═══════════════════════════════════════════════════════════ */

static char **split_lines(char *data, size_t size, size_t *nl) {
    size_t n = 0; for (size_t i = 0; i < size; i++) if (data[i] == '\n') n++;
    if (size > 0 && data[size-1] != '\n') n++;
    char **l = malloc(n * sizeof(char *)); if (!l) { *nl = 0; return NULL; }
    size_t li = 0, st = 0;
    for (size_t i = 0; i < size; i++) if (data[i] == '\n') { l[li++] = data + st; data[i] = '\0'; st = i + 1; }
    if (st < size) l[li++] = data + st;
    *nl = n; return l;
}

typedef struct { uint64_t hash; size_t idx; } lh_t;

static char *diff_unified(const char *ad, size_t as, const char *bd, size_t bs, const char *path, size_t ctx) {
    char *ac = malloc(as+1), *bc = malloc(bs+1);
    if (!ac || !bc) { free(ac); free(bc); return NULL; }
    memcpy(ac, ad, as); ac[as] = '\0'; memcpy(bc, bd, bs); bc[bs] = '\0';
    size_t an, bn; char **al = split_lines(ac, as, &an), **bl = split_lines(bc, bs, &bn);
    if (!al || !bl) { free(al); free(bl); free(ac); free(bc); return NULL; }

    /* Hash every line in version B for fast lookup */
    lh_t *bhash = malloc(bn * sizeof(lh_t));
    if (bhash) { for (size_t j = 0; j < bn; j++) { bhash[j].hash = pe_hash(bl[j]); bhash[j].idx = j; } }
    /* Simple scan: walk A, when lines differ find re-sync in B via hash lookup */
    size_t cap = as + bs + 4096; char *out = malloc(cap);
    if (!out) { free(bhash); free(al); free(bl); free(ac); free(bc); return NULL; }
    size_t ol = 0;
    #define O(...) do { int _n = snprintf(out+ol, cap-ol, __VA_ARGS__); if (_n<0) _n=0; \
        while (ol+(size_t)_n+1>cap) { cap*=2; char *_t=realloc(out,cap); if(!_t) goto oom; out=_t; \
        _n=snprintf(out+ol, cap-ol, __VA_ARGS__); if(_n<0) _n=0; } ol+=(size_t)_n; } while(0)

    O("--- a/%s\n+++ b/%s\n", path, path);

    size_t i = 0, j = 0;
    while (i < an || j < bn) {
        while (i < an && j < bn && strcmp(al[i], bl[j]) == 0) { i++; j++; }
        if (i >= an && j >= bn) break;
        size_t best_i = an, best_j = bn;
        /* Look ahead for re-sync (max 256 lines) */
        for (size_t k = 0; k < 256 && i + k < an; k++) {
            uint64_t h = pe_hash(al[i + k]);
            if (bhash) for (size_t m = j; m < bn && m < j + 256; m++)
                if (bhash[m].hash == h && strcmp(al[i+k], bl[m]) == 0) {
                    if (k + (m - j) < (best_i - i) + (best_j - j)) { best_i = i + k; best_j = m; }
                    break;
                }
        }
        size_t ds = i, de = best_i, is_ = j, ie = best_j;
        size_t ha = ds > ctx ? ds - ctx : 0, hb = is_ > ctx ? is_ - ctx : 0;
        size_t hea = de + ctx < an ? de + ctx : an, heb = ie + ctx < bn ? ie + ctx : bn;
        O("@@ -%zu,%zu +%zu,%zu @@\n", ha+1, hea-ha, hb+1, heb-hb);
        for (size_t k = ha; k < ds; k++) O(" %s\n", al[k]);
        for (size_t k = ds; k < de; k++) O("-%s\n", al[k]);
        for (size_t k = is_; k < ie; k++) O("+%s\n", bl[k]);
        for (size_t k = de; k < hea; k++) O(" %s\n", al[k]);
        i = hea; j = heb;
    }
    #undef O
    free(bhash); free(al); free(bl); free(ac); free(bc);
    return out;
oom:
    free(bhash); free(al); free(bl); free(ac); free(bc); free(out);
    return NULL;
}

char *pe_diff(pe_t *pe, const char *path, size_t ctx) {
    pthread_mutex_lock(&pe->sched_lock); pe_file_t *f = ht_find(pe, path); pthread_mutex_unlock(&pe->sched_lock);
    if (!f || !f->original) return NULL;
    pthread_rwlock_rdlock(&f->lock);
    char *r = diff_unified(f->original, f->original_size, f->data, f->size, path, ctx);
    pthread_rwlock_unlock(&f->lock);
    return r;
}

/* ═══════════════════════════════════════════════════════════
 * Transactions
 * ═══════════════════════════════════════════════════════════ */

int pe_txn_begin(pe_t *pe) {
    pthread_mutex_lock(&pe->sched_lock); if (pe->txn_active) { pthread_mutex_unlock(&pe->sched_lock); return -1; }
    pe->txn_active = true; pe->txn_nedits = 0; pthread_mutex_unlock(&pe->sched_lock); return 0;
}

size_t pe_txn_commit(pe_t *pe) {
    size_t nf = 0;
    pthread_mutex_lock(&pe->sched_lock);
    if (!pe->txn_active) { pthread_mutex_unlock(&pe->sched_lock); return 0; }
    for (size_t i = 0; i < pe->txn_nedits; i++) {
        pe_edit_t *ed = pe->txn_edits[i];
        if (!ed->file) { pe_file_t *f = ht_find(pe, ed->path); if (!f) { ed->error = strdup("no file"); nf++; continue; } ed->file = f; }
        pe->pending++;
        if (!ed->deps_remaining) ready_push(pe, ed);
        pthread_cond_signal(&pe->sched_cond);
    }
    pe->txn_active = false; pe->txn_nedits = 0;
    pthread_mutex_unlock(&pe->sched_lock);
    pe_flush(pe); return nf;
}

void pe_txn_rollback(pe_t *pe) {
    pthread_mutex_lock(&pe->sched_lock);
    if (!pe->txn_active) { pthread_mutex_unlock(&pe->sched_lock); return; }
    for (size_t i = 0; i < pe->txn_nedits; i++) pe_edit_free(pe->txn_edits[i]);
    pe->txn_nedits = 0; pe->txn_active = false;
    pthread_mutex_unlock(&pe->sched_lock);
}

bool pe_txn_active(pe_t *pe) { bool a; pthread_mutex_lock(&pe->sched_lock); a = pe->txn_active; pthread_mutex_unlock(&pe->sched_lock); return a; }

/* ═══════════════════════════════════════════════════════════
 * Branches
 * ═══════════════════════════════════════════════════════════ */

static pe_file_t *file_copy(pe_file_t *src) {
    pe_file_t *d = calloc(1, sizeof(pe_file_t)); if (!d) return NULL;
    d->path = strdup(src->path); d->path_hash = src->path_hash; d->size = src->size;
    if ((d->data = malloc(src->size+1))) { memcpy(d->data, src->data, src->size); d->data[src->size] = '\0'; }
    if (src->original && (d->original = malloc(src->original_size+1))) { memcpy(d->original, src->original, src->original_size); d->original[src->original_size] = '\0'; d->original_size = src->original_size; }
    d->nlines = src->nlines; if ((d->lines = malloc(src->nlines * sizeof(size_t)))) memcpy(d->lines, src->lines, src->nlines * sizeof(size_t));
    atomic_init(&d->dirty, atomic_load(&src->dirty)); pthread_rwlock_init(&d->lock, NULL);
    return d;
}

static int branch_snap(pe_t *pe, pe_branch_t *br) {
    br->cap = pe->files_cap ? pe->files_cap : 64;
    br->files = calloc(br->cap, sizeof(pe_file_t *)); if (!br->files) return -1;
    br->tomb = calloc(1, sizeof(pe_file_t)); if (!br->tomb) { free(br->files); return -1; }
    for (size_t i = 0; i < pe->files_cap; i++) {
        pe_file_t *f = pe->files[i]; if (!f || f == pe->tombstone) continue;
        pe_file_t *c = file_copy(f); if (!c) continue;
        size_t idx = c->path_hash % br->cap;
        while (br->files[idx] && br->files[idx] != br->tomb) idx = (idx + 1) % br->cap;
        br->files[idx] = c; br->count++;
    }
    return 0;
}

static void branch_free(pe_branch_t *br) {
    if (!br->files) return;
    for (size_t i = 0; i < br->cap; i++) { pe_file_t *f = br->files[i]; if (!f || f == br->tomb) continue;
        pthread_rwlock_destroy(&f->lock); free(f->path); free(f->data); free(f->original); free(f->lines);
        if (f->undo_stack) { for (size_t j = 0; j < f->undo_depth; j++) free(f->undo_stack[j].old); }
        free(f->undo_stack); free(f); }
    free(br->files); free(br->tomb);
}

static size_t branch_find(pe_t *pe, const char *name) {
    for (size_t i = 0; i < pe->nbranches; i++) if (strcmp(pe->branches[i].name, name) == 0) return i;
    return SIZE_MAX;
}

int pe_branch_create(pe_t *pe, const char *name) {
    pthread_mutex_lock(&pe->sched_lock);
    if (branch_find(pe, name) != SIZE_MAX) { pthread_mutex_unlock(&pe->sched_lock); return -1; }
    if (pe->nbranches >= pe->branches_cap) { size_t nc = pe->branches_cap ? pe->branches_cap * 2 : 4;
        pe_branch_t *np = realloc(pe->branches, nc * sizeof(*pe->branches)); if (!np) { pthread_mutex_unlock(&pe->sched_lock); return -1; }
        pe->branches = np; pe->branches_cap = nc; }
    pe_branch_t *br = &pe->branches[pe->nbranches]; memset(br, 0, sizeof(*br));
    if (!(br->name = strdup(name)) || branch_snap(pe, br)) { free(br->name); pthread_mutex_unlock(&pe->sched_lock); return -1; }
    pe->nbranches++; pthread_mutex_unlock(&pe->sched_lock); return 0;
}

int pe_branch_switch(pe_t *pe, const char *name) {
    pthread_mutex_lock(&pe->sched_lock);
    if (pe->current_branch) {
        size_t bi = branch_find(pe, pe->current_branch);
        if (bi != SIZE_MAX) { branch_free(&pe->branches[bi]); memset(&pe->branches[bi], 0, sizeof(pe->branches[bi]));
            pe->branches[bi].name = strdup(pe->current_branch); branch_snap(pe, &pe->branches[bi]); }
    } else {
        size_t bi = branch_find(pe, "main");
        if (bi == SIZE_MAX) { pthread_mutex_unlock(&pe->sched_lock); pe_branch_create(pe, "main"); pthread_mutex_lock(&pe->sched_lock); bi = branch_find(pe, "main"); }
        if (bi != SIZE_MAX) { branch_free(&pe->branches[bi]); memset(&pe->branches[bi], 0, sizeof(pe->branches[bi]));
            pe->branches[bi].name = strdup("main"); branch_snap(pe, &pe->branches[bi]); }
    }
    size_t bi = branch_find(pe, name);
    if (bi == SIZE_MAX) { pthread_mutex_unlock(&pe->sched_lock); return -1; }
    for (size_t i = 0; i < pe->files_cap; i++) { pe_file_t *f = pe->files[i]; if (!f || f == pe->tombstone) continue;
        pthread_rwlock_destroy(&f->lock); free(f->path); free(f->data); free(f->original); free(f->lines);
        if (f->undo_stack) { for (size_t j = 0; j < f->undo_depth; j++) free(f->undo_stack[j].old); } free(f->undo_stack); free(f); }
    free(pe->files); free(pe->tombstone);
    pe->files = pe->branches[bi].files; pe->files_cap = pe->branches[bi].cap; pe->files_count = pe->branches[bi].count; pe->tombstone = pe->branches[bi].tomb;
    pe->branches[bi].files = NULL; pe->branches[bi].cap = 0; pe->branches[bi].count = 0; pe->branches[bi].tomb = NULL;
    free(pe->current_branch); pe->current_branch = strdup(name);
    pthread_mutex_unlock(&pe->sched_lock); return 0;
}

static char *merge3(const char *o, size_t os, const char *a, size_t as, const char *b, size_t bs, int *cf) {
    if (!o) { o = ""; os = 0; }
    bool ac = (as != os || memcmp(o, a, os < as ? os : as)); bool bc = (bs != os || memcmp(o, b, os < bs ? os : bs));
    if (!ac && !bc) { char *r = malloc(as+1); if (r) { memcpy(r, a, as); r[as] = '\0'; } return r; }
    if (!bc) { char *r = malloc(as+1); if (r) { memcpy(r, a, as); r[as] = '\0'; } return r; }
    if (!ac) { char *r = malloc(bs+1); if (r) { memcpy(r, b, bs); r[bs] = '\0'; } return r; }
    char *oc = malloc(os+1), *ac2 = malloc(as+1), *bc2 = malloc(bs+1);
    if (!oc || !ac2 || !bc2) { free(oc); free(ac2); free(bc2); return NULL; }
    memcpy(oc, o, os); oc[os] = '\0'; memcpy(ac2, a, as); ac2[as] = '\0'; memcpy(bc2, b, bs); bc2[bs] = '\0';
    size_t on, an, bn; char **ol = split_lines(oc, os, &on), **al = split_lines(ac2, as, &an), **bl = split_lines(bc2, bs, &bn);
    if (!ol || !al || !bl) { free(ol); free(al); free(bl); free(oc); free(ac2); free(bc2); return NULL; }
    size_t cap = as+bs+4096; char *out = malloc(cap); if (!out) { free(ol); free(al); free(bl); free(oc); free(ac2); free(bc2); return NULL; }
    size_t ol2 = 0;
    #define MO(...) do { int _n = snprintf(out+ol2, cap-ol2, __VA_ARGS__); if (_n<0) _n=0; \
        while (ol2+(size_t)_n+1>cap) { cap*=2; char *_t=realloc(out,cap); if(!_t) goto moom; out=_t; \
        _n=snprintf(out+ol2, cap-ol2, __VA_ARGS__); if(_n<0) _n=0; } ol2+=(size_t)_n; } while(0)
    size_t oi=0, ai=0, bi=0;
    while (oi < on || ai < an || bi < bn) {
        while (oi < on && ai < an && bi < bn && strcmp(ol[oi], al[ai])==0 && strcmp(ol[oi], bl[bi])==0) { MO("%s\n", ol[oi]); oi++; ai++; bi++; }
        if (oi >= on && ai >= an && bi >= bn) break;
        if (ai < an && bi < bn && oi < on && strcmp(al[ai], ol[oi]) && !strcmp(bl[bi], ol[oi])) { MO("%s\n", al[ai]); ai++; oi++; bi++; continue; }
        if (bi < bn && ai < an && oi < on && strcmp(bl[bi], ol[oi]) && !strcmp(al[ai], ol[oi])) { MO("%s\n", bl[bi]); bi++; oi++; ai++; continue; }
        if (ai < an && bi < bn && oi < on && strcmp(al[ai], ol[oi]) && strcmp(bl[bi], ol[oi])) {
            MO("<<<<<<< A\n"); while (ai < an && (oi >= on || strcmp(al[ai], ol[oi]))) { MO("%s\n", al[ai]); ai++; }
            MO("=======\n"); while (bi < bn && (oi >= on || strcmp(bl[bi], ol[oi]))) { MO("%s\n", bl[bi]); bi++; }
            MO(">>>>>>> B\n"); *cf = 1; continue; }
        if (ai < an) { MO("%s\n", al[ai]); ai++; } else if (bi < bn) { MO("%s\n", bl[bi]); bi++; }
        if (oi < on) oi++;
    }
    #undef MO
    free(ol); free(al); free(bl); free(oc); free(ac2); free(bc2); return out;
moom: free(ol); free(al); free(bl); free(oc); free(ac2); free(bc2); free(out); return NULL;
}

int pe_branch_merge(pe_t *pe, const char *from, const char *to) {
    pthread_mutex_lock(&pe->sched_lock); size_t fi = branch_find(pe, from), ti = branch_find(pe, to);
    if (fi == SIZE_MAX || ti == SIZE_MAX) { pthread_mutex_unlock(&pe->sched_lock); return -1; }
    int cf = 0; pe_branch_t *fb = &pe->branches[fi];
    if (!fb->files) { pthread_mutex_unlock(&pe->sched_lock); return 0; }
    for (size_t i = 0; i < fb->cap; i++) { pe_file_t *ff = fb->files[i]; if (!ff || ff == fb->tomb) continue;
        pe_file_t *tf = ht_find(pe, ff->path);
        if (!tf) { pe_file_t *cp = file_copy(ff); if (cp) { if (!pe->files_cap || pe->files_count*100 >= pe->files_cap*70) ht_grow(pe);
            size_t idx = cp->path_hash % pe->files_cap; while (pe->files[idx] && pe->files[idx] != pe->tombstone) idx=(idx+1)%pe->files_cap;
            pe->files[idx] = cp; pe->files_count++; } continue; }
        bool fc = (!ff->original || ff->size != ff->original_size || memcmp(ff->data, ff->original, ff->size));
        bool tc = (!tf->original || tf->size != tf->original_size || memcmp(tf->data, tf->original, tf->size));
        if (!fc && !tc) continue; if (!fc) continue;
        if (!tc) { pthread_rwlock_wrlock(&tf->lock); free(tf->data); free(tf->lines);
            if (tf->undo_stack) { for (size_t j=0; j<tf->undo_depth; j++) free(tf->undo_stack[j].old); }
            free(tf->undo_stack); tf->undo_stack=NULL; tf->undo_depth=tf->undo_cap=0; tf->size=ff->size;
            tf->data=malloc(ff->size+1); if(tf->data){memcpy(tf->data,ff->data,ff->size);tf->data[ff->size]='\0';}
            tf->nlines=ff->nlines; tf->lines=malloc(ff->nlines*sizeof(size_t));
            if(tf->lines)memcpy(tf->lines,ff->lines,ff->nlines*sizeof(size_t));
            atomic_store(&tf->dirty,1); pthread_rwlock_unlock(&tf->lock); continue; }
        int fc2=0; char *m = merge3(tf->original, tf->original_size, ff->data, ff->size, tf->data, tf->size, &fc2);
        if (!m) continue; pthread_rwlock_wrlock(&tf->lock); free(tf->data); free(tf->lines);
        if (tf->undo_stack) { for (size_t j=0; j<tf->undo_depth; j++) free(tf->undo_stack[j].old); }
        free(tf->undo_stack); tf->undo_stack=NULL; tf->undo_depth=tf->undo_cap=0;
        tf->size=strlen(m); tf->data=m; build_lines(tf->data,tf->size,&tf->lines,&tf->nlines);
        atomic_store(&tf->dirty,1); pthread_rwlock_unlock(&tf->lock); if(fc2)cf++; }
    pthread_mutex_unlock(&pe->sched_lock); return cf ? 1 : 0;
}

int pe_branch_delete(pe_t *pe, const char *name) {
    pthread_mutex_lock(&pe->sched_lock);
    if ((pe->current_branch && !strcmp(pe->current_branch, name)) || (!pe->current_branch && !strcmp(name, "main"))) { pthread_mutex_unlock(&pe->sched_lock); return -1; }
    size_t bi = branch_find(pe, name); if (bi == SIZE_MAX) { pthread_mutex_unlock(&pe->sched_lock); return -1; }
    branch_free(&pe->branches[bi]); free(pe->branches[bi].name);
    for (size_t i = bi; i + 1 < pe->nbranches; i++) pe->branches[i] = pe->branches[i+1];
    pe->nbranches--; pthread_mutex_unlock(&pe->sched_lock); return 0;
}

size_t pe_branch_list(pe_t *pe, char ***names) {
    pthread_mutex_lock(&pe->sched_lock); size_t n = pe->nbranches + (!pe->current_branch ? 1 : 0);
    *names = malloc(n * sizeof(char *)); if (*names) { size_t j = 0;
        for (size_t i = 0; i < pe->nbranches; i++) (*names)[j++] = strdup(pe->branches[i].name);
        if (!pe->current_branch) (*names)[j++] = strdup("main"); }
    pthread_mutex_unlock(&pe->sched_lock); return n;
}

const char *pe_branch_current(pe_t *pe) { return pe->current_branch ? pe->current_branch : "main"; }

#endif /* PE_IMPL */

/* ═══════════════════════════════════════════════════════════
 * Daemon (Unix socket, single-threaded event loop)
 * ═══════════════════════════════════════════════════════════ */
#ifdef PE_DAEMON
#ifndef PE_IMPL
#error "PE_DAEMON requires PE_IMPL — define both before including pe.h"
#endif

#include <sys/socket.h>
#include <sys/un.h>
#include <sys/poll.h>
#include <signal.h>

#define DAEMON_SOCK  "/tmp/pe.sock"
#define MAX_CLI      64
#define BUF_SZ       65536
#define PATH_MAXLEN  4096
#define MAX_EDITS    4096

typedef enum { S_CMD, S_CONTENT } cst_t;

typedef struct {
    int fd; cst_t st; char *buf; size_t blen;
    char *content; size_t coff, ctotal;
    char cmd[32]; char path[PATH_MAXLEN];
    size_t line, col, eline, ecol, clen; int eid, did;
} client_t;

typedef struct { pe_edit_t *e; int id; bool sub; } eslot_t;

static pe_t *g_pe; static client_t *g_cl[MAX_CLI]; static size_t g_nc;
static eslot_t g_eds[MAX_EDITS]; static int g_nid = 1;
static int g_lfd = -1; static volatile sig_atomic_t g_run = 1;

static void on_sig(int s) { (void)s; g_run = 0; }

static client_t *cl_new(int fd) { client_t *c = calloc(1, sizeof(client_t)); if (!c) return NULL; c->fd = fd; c->buf = malloc(BUF_SZ); if (!c->buf) { free(c); return NULL; } return c; }
static void cl_free(client_t *c) { if (!c) return; if (c->fd >= 0) close(c->fd); free(c->buf); free(c->content); free(c); }

static void respond(client_t *c, const char *msg) { size_t l = strlen(msg); for (size_t o = 0; o < l; ) { ssize_t n = write(c->fd, msg+o, l-o); if (n<=0) break; o += (size_t)n; } }
__attribute__((format(printf,2,3))) static void respf(client_t *c, const char *fmt, ...) { char t[4096]; va_list a; va_start(a,fmt); int n = vsnprintf(t,sizeof(t),fmt,a); va_end(a); if (n>0) respond(c,t); }
static void resp_raw(client_t *c, const char *d, size_t l) { for (size_t o=0;o<l;){ssize_t n=write(c->fd,d+o,l-o);if(n<=0)break;o+=(size_t)n;} }

static const char *help_text =
    "PE DAEMON COMMANDS\n"
    "──────────────────\n"
    "CACHE <path>                    Load file into cache\n"
    "EXISTS <path>                   Check if file is cached\n"
    "INFO <path>                     Show line count and byte size\n"
    "SIZE <path>                     Show byte size only\n"
    "GET <path>                      Return full file contents\n"
    "INSERT <path> <L> <C> <len>\\n<bytes>  Insert content at line:col\n"
    "DELETE <path> <sl> <sc> <el> <ec>      Delete range [start, end)\n"
    "REPLACE <path> <sl> <sc> <el> <ec> <len>\\n<bytes>  Replace range\n"
    "DEP <id> <dep>                  Edit <id> depends on <dep>\n"
    "SUBMIT <id>                     Queue edit for execution\n"
    "FLUSH                           Wait for all edits to complete\n"
    "SYNC                            Write dirty files to disk\n"
    "DIFF <path> [ctx]               Unified diff (original vs current)\n"
    "UNDO <path>                     Undo last edit on file\n"
    "BEGIN                           Start transaction\n"
    "COMMIT                          Commit transaction (apply all queued)\n"
    "ROLLBACK                        Discard transaction\n"
    "BRANCH <name>                   Create named branch from current state\n"
    "SWITCH <name>                   Switch to branch\n"
    "MERGE <from> <to>               3-way merge from branch into target\n"
    "BRANCHES                        List branches (* = current)\n"
    "DELETE_BRANCH <name>            Delete a branch\n"
    "HELP                            This help text\n"
    "QUIT                            Close connection\n"
    "KILL                            Shutdown daemon\n";

static int parse_cmd(char *line, client_t *c) {
    c->path[0]='\0'; c->line=c->col=c->eline=c->ecol=c->clen=c->eid=c->did=0;
    char *tok=strtok(line," \t"); if(!tok)return -1;
    strncpy(c->cmd,tok,sizeof(c->cmd)-1); c->cmd[sizeof(c->cmd)-1]='\0';
    #define NEXT() (tok=strtok(NULL," \t"))
    #define NEED() do{if(!NEXT())return -1;}while(0)
    if(!strcmp(c->cmd,"CACHE")||!strcmp(c->cmd,"GET")||!strcmp(c->cmd,"INFO")||!strcmp(c->cmd,"EXISTS")||!strcmp(c->cmd,"SIZE")||!strcmp(c->cmd,"UNDO")||!strcmp(c->cmd,"BRANCH")||!strcmp(c->cmd,"SWITCH")||!strcmp(c->cmd,"DELETE_BRANCH")){NEED();strncpy(c->path,tok,PATH_MAXLEN-1);return 0;}
    if(!strcmp(c->cmd,"MERGE")){NEED();strncpy(c->path,tok,PATH_MAXLEN-1);return 0;}
    if(!strcmp(c->cmd,"DIFF")){NEED();strncpy(c->path,tok,PATH_MAXLEN-1);if(NEXT())c->line=strtoul(tok,NULL,10);return 0;}
    if(!strcmp(c->cmd,"FLUSH")||!strcmp(c->cmd,"SYNC")||!strcmp(c->cmd,"QUIT")||!strcmp(c->cmd,"KILL")||!strcmp(c->cmd,"BEGIN")||!strcmp(c->cmd,"COMMIT")||!strcmp(c->cmd,"ROLLBACK")||!strcmp(c->cmd,"BRANCHES")||!strcmp(c->cmd,"HELP"))return 0;
    if(!strcmp(c->cmd,"INSERT")||!strcmp(c->cmd,"REPLACE")){NEED();strncpy(c->path,tok,PATH_MAXLEN-1);NEED();c->line=strtoul(tok,NULL,10);NEED();c->col=strtoul(tok,NULL,10);
        if(!strcmp(c->cmd,"REPLACE")){NEED();c->eline=strtoul(tok,NULL,10);NEED();c->ecol=strtoul(tok,NULL,10);}NEED();c->clen=strtoul(tok,NULL,10);return 0;}
    if(!strcmp(c->cmd,"DELETE")){NEED();strncpy(c->path,tok,PATH_MAXLEN-1);NEED();c->line=strtoul(tok,NULL,10);NEED();c->col=strtoul(tok,NULL,10);NEED();c->eline=strtoul(tok,NULL,10);NEED();c->ecol=strtoul(tok,NULL,10);return 0;}
    if(!strcmp(c->cmd,"DEP")){NEED();c->eid=atoi(tok);NEED();c->did=atoi(tok);return 0;}
    if(!strcmp(c->cmd,"SUBMIT")){NEED();c->eid=atoi(tok);return 0;}
    #undef NEED
    #undef NEXT
    return -1;
}

static void exec_cmd(client_t *c) {
    if (!strcmp(c->cmd, "HELP")) { respond(c, help_text); return; }
    if (!strcmp(c->cmd, "CACHE")) { respond(c, pe_cache_file(g_pe,c->path)==0?"OK\n":"ERR\n"); return; }
    if (!strcmp(c->cmd, "EXISTS")) { respond(c, pe_file_exists(g_pe,c->path)?"YES\n":"NO\n"); return; }
    if (!strcmp(c->cmd, "SIZE")) { size_t sz = pe_file_size(g_pe,c->path); if(!sz && !pe_file_exists(g_pe,c->path)){respf(c,"ERR %s\n",c->path);return;} respf(c,"SIZE %zu\n",sz); return; }
    if (!strcmp(c->cmd, "FLUSH")) { respf(c, "FLUSHED %zu\n", pe_flush(g_pe)); return; }
    if (!strcmp(c->cmd, "SYNC")) { respond(c, pe_sync(g_pe)==0?"SYNCED 0\n":"ERR sync\n"); return; }
    if (!strcmp(c->cmd, "QUIT")) { respond(c, "BYE\n"); c->fd = -2; return; }
    if (!strcmp(c->cmd, "KILL")) { respond(c, "BYE\n"); g_run = 0; return; }
    if (!strcmp(c->cmd, "GET")) { size_t sz; const char *d = pe_file_data(g_pe,c->path,&sz); if(!d){respf(c,"ERR %s\n",c->path);return;} respf(c,"DATA %zu\n",sz); resp_raw(c,d,sz); return; }
    if (!strcmp(c->cmd, "INFO")) { size_t sz; if(!pe_file_data(g_pe,c->path,&sz)){respf(c,"ERR %s\n",c->path);return;} respf(c,"INFO %zu %zu\n",pe_file_lines(g_pe,c->path),sz); return; }
    if (!strcmp(c->cmd, "DIFF")) { size_t ctx=c->line?c->line:3; char *d=pe_diff(g_pe,c->path,ctx); if(!d){respf(c,"ERR %s\n",c->path);return;} if(d[0]){respf(c,"DIFF %zu\n",strlen(d));resp_raw(c,d,strlen(d));}else respond(c,"DIFF 0\n"); free(d); return; }
    if (!strcmp(c->cmd, "UNDO")) { respond(c, pe_undo(g_pe,c->path)==0?"OK\n":"ERR undo\n"); return; }
    if (!strcmp(c->cmd, "BEGIN")) { respond(c, pe_txn_begin(g_pe)==0?"OK\n":"ERR txn\n"); return; }
    if (!strcmp(c->cmd, "COMMIT")) { respf(c, "COMMITTED %zu\n", pe_txn_commit(g_pe)); return; }
    if (!strcmp(c->cmd, "ROLLBACK")) { pe_txn_rollback(g_pe); respond(c, "OK\n"); return; }
    if (!strcmp(c->cmd, "BRANCH")) { respond(c, pe_branch_create(g_pe,c->path)==0?"OK\n":"ERR exists\n"); return; }
    if (!strcmp(c->cmd, "SWITCH")) { respond(c, pe_branch_switch(g_pe,c->path)==0?"OK\n":"ERR not found\n"); return; }
    if (!strcmp(c->cmd, "MERGE")) { char tb[PATH_MAXLEN]; char *t=strtok(NULL," \t\r\n"); if(t)strncpy(tb,t,sizeof(tb)-1);else{respond(c,"ERR usage: MERGE from to\n");return;}
        int rc=pe_branch_merge(g_pe,c->path,tb); respond(c, rc==0?"OK\n":rc==1?"CONFLICTS\n":"ERR\n"); return; }
    if (!strcmp(c->cmd, "BRANCHES")) { char **n; size_t nn=pe_branch_list(g_pe,&n); respf(c,"BRANCHES %zu\n",nn);
        for(size_t i=0;i<nn;i++){respf(c,"%s%s\n",n[i],strcmp(n[i],pe_branch_current(g_pe))==0?" *":"");free(n[i]);} free(n); return; }
    if (!strcmp(c->cmd, "DELETE_BRANCH")) { respond(c, pe_branch_delete(g_pe,c->path)==0?"OK\n":"ERR\n"); return; }
    if (!strcmp(c->cmd, "INSERT") || !strcmp(c->cmd, "REPLACE") || !strcmp(c->cmd, "DELETE")) {
        if (g_nid >= MAX_EDITS) { respond(c, "ERR full\n"); return; }
        pe_op_t op = !strcmp(c->cmd,"INSERT")?PE_OP_INSERT:!strcmp(c->cmd,"DELETE")?PE_OP_DELETE:PE_OP_REPLACE;
        pe_edit_t *e = pe_edit_create(g_pe, c->path, op, (pe_pos_t){c->line,c->col}, (pe_pos_t){c->eline,c->ecol}, c->content, c->ctotal);
        if (!e) { respond(c, "ERR create\n"); return; }
        int id = g_nid++; g_eds[id].e = e; g_eds[id].id = id; g_eds[id].sub = false; respf(c, "OK %d\n", id); return; }
    if (!strcmp(c->cmd, "DEP")) { if (c->eid<=0||c->eid>=g_nid||c->did<=0||c->did>=g_nid||!g_eds[c->eid].e||!g_eds[c->did].e){respond(c,"ERR bad id\n");return;}
        if (pe_edit_depend(g_eds[c->eid].e, g_eds[c->did].e)!=0){respond(c,"ERR dep\n");return;} respond(c,"OK\n"); return; }
    if (!strcmp(c->cmd, "SUBMIT")) { if (c->eid<=0||c->eid>=g_nid||!g_eds[c->eid].e){respond(c,"ERR bad id\n");return;}
        if (g_eds[c->eid].sub){respond(c,"ERR dup\n");return;} if(pe_edit_submit(g_pe,g_eds[c->eid].e)!=0){respond(c,"ERR submit\n");return;}
        g_eds[c->eid].sub=true; respond(c,"OK\n"); return; }
    respf(c, "ERR unknown: %s\n", c->cmd);
}

static bool process_client(client_t *c) {
    if (c->blen > BUF_SZ - 4096) { size_t nc = c->blen + BUF_SZ; char *nb = realloc(c->buf, nc); if (!nb) return false; c->buf = nb; }
    ssize_t nr = read(c->fd, c->buf + c->blen, BUF_SZ - 1); if (nr <= 0) return false;
    c->blen += (size_t)nr; size_t pos = 0;
    while (pos < c->blen) {
        if (c->st == S_CMD) { char *nl = memchr(c->buf + pos, '\n', c->blen - pos); if (!nl) break;
            *nl = '\0'; char *line = c->buf + pos; pos = (size_t)((nl + 1) - c->buf);
            if (line[0] == '\0') continue;
            if (parse_cmd(line, c) != 0) { respf(c, "ERR parse\n"); continue; }
            if (c->clen > 0) { c->st = S_CONTENT; c->ctotal = c->clen; c->coff = 0; free(c->content);
                c->content = malloc(c->clen + 1); if (!c->content) { respond(c, "ERR oom\n"); return false; } }
            else { exec_cmd(c); if (c->fd == -2) { close(c->fd); c->fd = -1; return false; } if (!g_run) return true; continue; } }
        { size_t av = c->blen - pos, nd = c->ctotal - c->coff, tk = av < nd ? av : nd;
            memcpy(c->content + c->coff, c->buf + pos, tk); c->coff += tk; pos += tk;
            if (c->coff >= c->ctotal) { c->content[c->ctotal] = '\0'; exec_cmd(c);
                free(c->content); c->content = NULL; c->st = S_CMD; c->clen = 0;
                if (c->fd == -2) { close(c->fd); c->fd = -1; return false; } if (!g_run) return true; }
            else break; }
    }
    if (pos > 0 && pos < c->blen) memmove(c->buf, c->buf + pos, c->blen - pos);
    if (pos <= c->blen) c->blen -= pos;
    return true;
}

#ifdef PE_DAEMON_MAIN
int main(int argc, char **argv) {
    const char *sock = (argc > 1) ? argv[1] : DAEMON_SOCK;
    const char *root = (argc > 2) ? argv[2] : ".";
    g_pe = pe_create(root, 0); if (!g_pe) { fprintf(stderr, "pe_create failed\n"); return 1; }
    signal(SIGINT, on_sig); signal(SIGTERM, on_sig); signal(SIGPIPE, SIG_IGN);
    g_lfd = socket(AF_UNIX, SOCK_STREAM, 0); if (g_lfd < 0) { perror("socket"); pe_destroy(g_pe); return 1; }
    { int one = 1; setsockopt(g_lfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one)); }
    struct sockaddr_un addr = {.sun_family = AF_UNIX}; strncpy(addr.sun_path, sock, sizeof(addr.sun_path)-1); unlink(sock);
    if (bind(g_lfd, (struct sockaddr*)&addr, sizeof(addr)) < 0) { perror("bind"); close(g_lfd); pe_destroy(g_pe); return 1; }
    chmod(sock, 0600);
    if (listen(g_lfd, 16) < 0) { perror("listen"); unlink(sock); close(g_lfd); pe_destroy(g_pe); return 1; }
    printf("ped: %s (root: %s)\n", sock, root);
    struct pollfd fds[MAX_CLI+1];
    while (g_run) { int nfds = 0; fds[nfds].fd = g_lfd; fds[nfds].events = POLLIN; nfds++;
        for (size_t i = 0; i < g_nc; i++) { fds[nfds].fd = g_cl[i]->fd; fds[nfds].events = POLLIN; nfds++; }
        if (poll(fds, nfds, 500) < 0) { if (errno == EINTR) continue; perror("poll"); break; }
        if (fds[0].revents & POLLIN) { int cfd = accept(g_lfd, NULL, NULL); if (cfd >= 0) {
            if (g_nc < MAX_CLI) { client_t *nc = cl_new(cfd); if (nc) { g_cl[g_nc++] = nc; respond(nc, "READY\n"); } else close(cfd); }
            else close(cfd); } }
        for (size_t i = g_nc; i > 0; ) { i--; if (fds[i+1].revents & (POLLIN|POLLHUP|POLLERR)) {
            if (!process_client(g_cl[i])) { cl_free(g_cl[i]); g_cl[i] = g_cl[--g_nc]; } } }
    }
    printf("ped: shutting down...\n");
    for (size_t i = 0; i < g_nc; i++) cl_free(g_cl[i]);
    close(g_lfd); unlink(sock); pe_destroy(g_pe); printf("ped: done\n"); return 0;
}
#endif /* PE_DAEMON_MAIN */
#endif /* PE_DAEMON */
