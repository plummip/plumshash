/*
 * pe.h — Project Editor: C library + daemon
 *
 * Zero-dependency (libc + pthreads).  API declarations.
 * Implementation in pe.c — link with pe.o.
 *
 *   gcc -std=c11 -O2 -c pe.c
 *   gcc -std=c11 -O2 -o mytool mytool.c pe.o -pthread
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
 *   - Shared-memory daemon (peshmd/peshmc)
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
