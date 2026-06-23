/*
 * peshmd.c — Project Editor Shared-Memory Daemon
 *
 * Creates a shared-memory region via open() + mmap(MAP_SHARED)
 * containing a ring buffer for commands and per-client response
 * slots.  Multi-process sync via PTHREAD_PROCESS_SHARED mutexes
 * and condition variables.  No external libs — libc + pthreads.
 *
 * Build:  gcc -std=c11 -Wall -O2 -o peshmd peshmd.c pe.o -pthread
 * Usage:  ./peshmd [project_root] [shm_file_path]
 */

#define _GNU_SOURCE
#include "pe_shm.h"
#include "pe.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <errno.h>
#include <signal.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>

/* Default shared memory file */
#ifndef PE_SHM_PATH
#define PE_SHM_PATH "/data/data/com.termux/files/home/tmp/pe_shm"
#endif

/* ── Ring-buffer copy helpers (handle wrap) ────────────── */

static inline size_t rb_off(uint64_t c) {
    return (size_t)(c & (PE_SHM_RING_SIZE - 1));
}

static void rb_read(const pe_shm_t *s, size_t off, void *dst, size_t len) {
    size_t first = PE_SHM_RING_SIZE - off;
    if (len <= first) {
        memcpy(dst, s->cmd_buf + off, len);
    } else {
        memcpy(dst, s->cmd_buf + off, first);
        memcpy((char *)dst + first, s->cmd_buf, len - first);
    }
}

/* ── Globals ───────────────────────────────────────────── */

static pe_t      *g_pe;
static pe_shm_t  *g_shm;
static int        g_shm_fd = -1;
static const char *g_shm_path = PE_SHM_PATH;
static volatile sig_atomic_t g_running = 1;

/* ── Edit table ────────────────────────────────────────── */

#define MAX_EDITS 4096
typedef struct { pe_edit_t *e; int id; bool submitted; } eslot_t;
static eslot_t g_edits[MAX_EDITS];
static int     g_next_id = 1;

/* ── Signal ────────────────────────────────────────────── */

static void on_signal(int s) { (void)s; g_running = 0; }

/* ── Response helpers ──────────────────────────────────── */

static void resp_write(int slot, uint32_t status,
                       const char *data, size_t dlen) {
    pe_resp_slot_t *r = &g_shm->slots[slot];
    pthread_mutex_lock(&r->lock);
    r->status   = status;
    r->data_len = (dlen < PE_SHM_RESP_SIZE) ? (uint32_t)dlen
                                            : (uint32_t)(PE_SHM_RESP_SIZE - 1);
    if (data && r->data_len > 0)
        memcpy(r->data, data, r->data_len);
    r->data[r->data_len] = '\0';
    r->has_resp = 1;
    pthread_cond_signal(&r->cond);
    pthread_mutex_unlock(&r->lock);
}

static void resp_ok(int slot, const char *data) {
    resp_write(slot, 0, data, strlen(data));
}

static void resp_err(int slot, const char *msg) {
    resp_write(slot, 1, msg, strlen(msg));
}

__attribute__((format(printf, 2, 3)))
static void resp_fmt(int slot, const char *fmt, ...) {
    char tmp[PE_SHM_RESP_SIZE];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(tmp, sizeof(tmp), fmt, ap);
    va_end(ap);
    resp_write(slot, 0, tmp, (size_t)(n > 0 ? n : 0));
}

/* ── Command processing (reuses pe library) ────────────── */

static void process_cmd(const pe_cmd_hdr_t *hdr,
                        const char *path, const char *content) {
    int sid = (int)hdr->client_id;
    if (sid <= 0 || sid > PE_SHM_MAX_CLIENTS) return;

    switch (hdr->cmd_type) {

    case PE_CMD_CACHE:
        resp_write(sid, pe_cache_file(g_pe, path) == 0 ? 0 : 1, "OK\n", 3);
        break;

    case PE_CMD_FLUSH: {
        size_t nf = pe_flush(g_pe);
        resp_fmt(sid, "FLUSHED %zu\n", nf);
        break;
    }

    case PE_CMD_SYNC:
        resp_write(sid, pe_sync(g_pe) == 0 ? 0 : 1, "SYNCED 0\n", 9);
        break;

    case PE_CMD_GET: {
        size_t sz;
        const char *data = pe_file_data(g_pe, path, &sz);
        if (!data) { resp_fmt(sid, "ERR %s\n", path); break; }
        char head[64];
        int hn = snprintf(head, sizeof(head), "DATA %zu\n", sz);
        size_t total = (size_t)hn + sz;
        char *buf = malloc(total + 1);
        if (!buf) { resp_err(sid, "ERR oom\n"); break; }
        memcpy(buf, head, (size_t)hn);
        memcpy(buf + hn, data, sz);
        resp_write(sid, 0, buf, total);
        free(buf);
        break;
    }

    case PE_CMD_INFO: {
        size_t sz;
        if (!pe_file_data(g_pe, path, &sz))
            { resp_fmt(sid, "ERR %s\n", path); break; }
        resp_fmt(sid, "INFO %zu %zu\n", pe_file_lines(g_pe, path), sz);
        break;
    }

    case PE_CMD_INSERT:
    case PE_CMD_DELETE:
    case PE_CMD_REPLACE: {
        if (g_next_id >= MAX_EDITS) { resp_err(sid, "ERR full\n"); break; }

        pe_op_t op = (hdr->cmd_type == PE_CMD_INSERT) ? PE_OP_INSERT
                   : (hdr->cmd_type == PE_CMD_DELETE) ? PE_OP_DELETE
                   : PE_OP_REPLACE;

        pe_edit_t *e = pe_edit_create(g_pe, path, op,
            (pe_pos_t){(size_t)hdr->start_line, (size_t)hdr->start_col},
            (pe_pos_t){(size_t)hdr->end_line,   (size_t)hdr->end_col},
            content, (size_t)hdr->content_len);
        if (!e) { resp_err(sid, "ERR create\n"); break; }

        int id = g_next_id++;
        g_edits[id].e         = e;
        g_edits[id].id        = id;
        g_edits[id].submitted = false;
        resp_fmt(sid, "OK %d\n", id);
        break;
    }

    case PE_CMD_DEP: {
        int eid = (int)hdr->edit_id, did = (int)hdr->dep_id;
        if (eid <= 0 || eid >= g_next_id || did <= 0 || did >= g_next_id ||
            !g_edits[eid].e || !g_edits[did].e)
            { resp_err(sid, "ERR bad id\n"); break; }
        if (pe_edit_depend(g_edits[eid].e, g_edits[did].e) != 0)
            { resp_err(sid, "ERR dep\n"); break; }
        resp_ok(sid, "OK\n");
        break;
    }

    case PE_CMD_SUBMIT: {
        int eid = (int)hdr->edit_id;
        if (eid <= 0 || eid >= g_next_id || !g_edits[eid].e)
            { resp_err(sid, "ERR bad id\n"); break; }
        if (g_edits[eid].submitted)
            { resp_err(sid, "ERR dup\n"); break; }
        if (pe_edit_submit(g_pe, g_edits[eid].e) != 0)
            { resp_err(sid, "ERR submit\n"); break; }
        g_edits[eid].submitted = true;
        resp_ok(sid, "OK\n");
        break;
    }

    case PE_CMD_QUIT:
        resp_ok(sid, "BYE\n");
        g_shm->slots[sid].in_use = 0;
        break;

    case PE_CMD_KILL:
        resp_ok(sid, "BYE\n");
        g_running = 0;
        break;

    default:
        resp_err(sid, "ERR unknown\n");
        break;
    }
}

/* ── Daemon main loop ──────────────────────────────────── */

static void daemon_loop(void) {
    char path[4096];
    char content[65536];

    while (g_running) {
        pthread_mutex_lock(&g_shm->cmd_lock);

        while (g_running && g_shm->cmd_head == g_shm->cmd_tail)
            pthread_cond_wait(&g_shm->cmd_cond, &g_shm->cmd_lock);

        if (!g_running) {
            pthread_mutex_unlock(&g_shm->cmd_lock);
            break;
        }

        while (g_shm->cmd_head != g_shm->cmd_tail && g_running) {
            size_t off = rb_off(g_shm->cmd_head);

            uint32_t total;
            rb_read(g_shm, off, &total, 4);

            if (total == 0 || total > PE_SHM_RING_SIZE) {
                /* Padding or corrupt — skip to buffer start */
                size_t skip = PE_SHM_RING_SIZE -
                    (size_t)(g_shm->cmd_head & (PE_SHM_RING_SIZE - 1));
                g_shm->cmd_head += skip ? skip : PE_SHM_RING_SIZE;
                continue;
            }

            off = rb_off(g_shm->cmd_head + 4);

            pe_cmd_hdr_t hdr;
            rb_read(g_shm, off, &hdr, sizeof(hdr));

            size_t plen = hdr.path_len;
            if (plen >= sizeof(path)) plen = sizeof(path) - 1;
            rb_read(g_shm, rb_off(g_shm->cmd_head + 4 + sizeof(hdr)),
                    path, plen);
            path[plen] = '\0';

            size_t clen = hdr.content_len;
            if (clen >= sizeof(content)) clen = sizeof(content) - 1;
            rb_read(g_shm, rb_off(g_shm->cmd_head + 4 + sizeof(hdr) + plen),
                    content, clen);
            content[clen] = '\0';

            g_shm->cmd_head += total;

            pthread_mutex_unlock(&g_shm->cmd_lock);
            process_cmd(&hdr, path, content);
            pthread_mutex_lock(&g_shm->cmd_lock);
        }

        pthread_mutex_unlock(&g_shm->cmd_lock);
    }
}

/* ── Init shared memory ────────────────────────────────── */

static int shm_init(const char *proj_root) {
    /* Remove stale file */
    unlink(g_shm_path);

    g_shm_fd = open(g_shm_path, O_RDWR | O_CREAT | O_EXCL, 0600);
    if (g_shm_fd < 0) {
        perror("open shm file");
        return -1;
    }

    if (ftruncate(g_shm_fd, (off_t)sizeof(pe_shm_t)) < 0) {
        perror("ftruncate");
        close(g_shm_fd); unlink(g_shm_path);
        return -1;
    }

    g_shm = mmap(NULL, sizeof(pe_shm_t), PROT_READ | PROT_WRITE,
                 MAP_SHARED, g_shm_fd, 0);
    if (g_shm == MAP_FAILED) {
        perror("mmap");
        close(g_shm_fd); unlink(g_shm_path);
        return -1;
    }
    close(g_shm_fd);
    g_shm_fd = -1;

    /* Zero and init */
    memset(g_shm, 0, sizeof(pe_shm_t));
    g_shm->magic   = PE_SHM_MAGIC;
    g_shm->version = PE_SHM_VERSION;

    /* Init process-shared mutexes & condvars */
    pthread_mutexattr_t ma;
    pthread_mutexattr_init(&ma);
    pthread_mutexattr_setpshared(&ma, PTHREAD_PROCESS_SHARED);

    pthread_condattr_t ca;
    pthread_condattr_init(&ca);
    pthread_condattr_setpshared(&ca, PTHREAD_PROCESS_SHARED);

    pthread_mutex_init(&g_shm->cmd_lock, &ma);
    pthread_cond_init(&g_shm->cmd_cond, &ca);
    pthread_mutex_init(&g_shm->reg_lock, &ma);

    for (int i = 1; i <= PE_SHM_MAX_CLIENTS; i++) {
        pthread_mutex_init(&g_shm->slots[i].lock, &ma);
        pthread_cond_init(&g_shm->slots[i].cond, &ca);
    }

    pthread_mutexattr_destroy(&ma);
    pthread_condattr_destroy(&ca);

    /* Create project editor */
    g_pe = pe_create(proj_root, 0);
    if (!g_pe) {
        fprintf(stderr, "peshmd: pe_create failed\n");
        return -1;
    }

    printf("peshmd: shm at %s, root=%s, %d slots, ring=%dKB\n",
           g_shm_path, proj_root, PE_SHM_MAX_CLIENTS,
           PE_SHM_RING_SIZE / 1024);
    return 0;
}

static void shm_cleanup(void) {
    if (g_pe) pe_destroy(g_pe);
    if (g_shm && g_shm != MAP_FAILED) {
        for (int i = 1; i <= PE_SHM_MAX_CLIENTS; i++) {
            pthread_mutex_destroy(&g_shm->slots[i].lock);
            pthread_cond_destroy(&g_shm->slots[i].cond);
        }
        pthread_cond_destroy(&g_shm->cmd_cond);
        pthread_mutex_destroy(&g_shm->cmd_lock);
        pthread_mutex_destroy(&g_shm->reg_lock);
        munmap(g_shm, sizeof(pe_shm_t));
    }
    if (g_shm_fd >= 0) close(g_shm_fd);
    unlink(g_shm_path);
}

/* ── Main ──────────────────────────────────────────────── */

int main(int argc, char **argv) {
    const char *root = (argc > 1) ? argv[1] : ".";

    signal(SIGINT,  on_signal);
    signal(SIGTERM, on_signal);
    signal(SIGPIPE, SIG_IGN);

    if (shm_init(root) != 0) return 1;

    daemon_loop();

    printf("peshmd: shutting down...\n");
    shm_cleanup();
    printf("peshmd: done\n");
    return 0;
}
