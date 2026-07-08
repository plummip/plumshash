/*
 * pe_shm.h — Shared-memory layout for the Project Editor daemon
 *
 * The daemon creates a shared memory region (/dev/shm/pe_ctl)
 * containing a MPSC ring buffer for commands and per-client
 * response slots.  Multi-process synchronization via
 * PTHREAD_PROCESS_SHARED mutexes and condition variables
 * (no external libraries — just libc + pthreads).
 */

#ifndef PE_SHM_H
#define PE_SHM_H

#include <stdint.h>
#include <stddef.h>
#include <pthread.h>
#include <unistd.h>

/* ── Constants ─────────────────────────────────────────── */

#define PE_SHM_MAGIC      0x5045515545554541ULL  /* "PEQUEUEA"      */
#define PE_SHM_VERSION    1
#define PE_SHM_NAME       "/pe_ctl"
#define PE_SHM_RING_SIZE  (256 * 1024)            /* 256 KB ring    */
#define PE_SHM_MAX_CLIENTS 16
#define PE_SHM_RESP_SIZE  8192                   /* per-client resp */

/* ── Command types ─────────────────────────────────────── */

enum {
    PE_CMD_CACHE   = 1,
    PE_CMD_INSERT  = 2,
    PE_CMD_DELETE  = 3,
    PE_CMD_REPLACE = 4,
    PE_CMD_DEP     = 5,
    PE_CMD_SUBMIT  = 6,
    PE_CMD_FLUSH   = 7,
    PE_CMD_SYNC    = 8,
    PE_CMD_GET     = 9,
    PE_CMD_INFO    = 10,
    PE_CMD_QUIT    = 11,
    PE_CMD_KILL    = 12,
    PE_CMD_UNDO    = 13,
    PE_CMD_DIFF    = 14,
    PE_CMD_BRANCH  = 15,
    PE_CMD_SWITCH  = 16,
    PE_CMD_MERGE   = 17,
    PE_CMD_BRANCHES = 18,
    PE_CMD_DELBR   = 19,
    PE_CMD_BEGIN   = 20,
    PE_CMD_COMMIT  = 21,
    PE_CMD_ROLLBACK = 22
};

/* ── Packed command header (60 bytes) ──────────────────── */

typedef struct __attribute__((packed)) {
    uint32_t total_len;     /* including this field + padding       */
    uint32_t cmd_type;      /* PE_CMD_*                             */
    uint32_t client_id;     /* which slot (1-based)                 */
    uint32_t edit_id;       /* for DEP/SUBMIT                       */
    uint32_t dep_id;        /* for DEP                              */
    uint32_t path_len;      /* bytes of path following header       */
    uint32_t content_len;   /* bytes of content following path      */
    uint64_t start_line;
    uint64_t start_col;
    uint64_t end_line;
    uint64_t end_col;
    /* Followed inline by: path[path_len] + content[content_len]    */
    /* Followed by padding to 8-byte alignment                      */
} pe_cmd_hdr_t;

/* ── Per-client response slot ──────────────────────────── */

typedef struct {
    int             in_use;     /* 1 = slot taken                    */
    pid_t           pid;        /* client PID (for dead-client GC)   */
    pthread_mutex_t lock;
    pthread_cond_t  cond;
    int             has_resp;   /* 1 = response ready for client     */
    uint32_t        status;     /* 0 = OK                            */
    uint32_t        data_len;   /* bytes in data[]                   */
    char            data[PE_SHM_RESP_SIZE];
} pe_resp_slot_t;

/* ── Top-level shared memory region ────────────────────── */

typedef struct {
    uint64_t magic;              /* PE_SHM_MAGIC                    */
    uint32_t version;
    uint32_t _pad0;

    /* ── Command ring ──────────────────────────────────── */
    pthread_mutex_t cmd_lock;    /* protects head/tail/ring        */
    pthread_cond_t  cmd_cond;    /* daemon waits; clients signal   */
    uint64_t        cmd_head;    /* daemon reads from here         */
    uint64_t        cmd_tail;    /* clients write here (with lock) */
    uint8_t         cmd_buf[PE_SHM_RING_SIZE] __attribute__((aligned(8)));

    /* ── Client registration ───────────────────────────── */
    pthread_mutex_t reg_lock;    /* protects next_slot, slots[].in_use */
    int             next_slot;   /* next free slot (1-based)       */

    /* ── Per-client response slots ─────────────────────── */
    /* slots[0] is unused (client_id 0 = invalid) */
    pe_resp_slot_t  slots[PE_SHM_MAX_CLIENTS + 1];

} pe_shm_t;

/* ── Ring-buffer helpers (caller holds cmd_lock) ───────── */

/* Return number of bytes available to read */
static inline size_t shm_ring_readable(const pe_shm_t *s) {
    uint64_t h = s->cmd_head;
    uint64_t t = s->cmd_tail;
    return (size_t)(t - h);
}

/* Return number of bytes available to write */
static inline size_t shm_ring_writable(const pe_shm_t *s) {
    uint64_t h = s->cmd_head;
    uint64_t t = s->cmd_tail;
    size_t used = (size_t)(t - h);
    return PE_SHM_RING_SIZE - used;
}

#endif /* PE_SHM_H */
