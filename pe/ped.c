/*
 * ped.c — Project Editor Daemon
 *
 * Persistent daemon listening on a Unix domain socket.
 * Multiple clients connect concurrently; edits interleave
 * safely through the pe library's thread pool + per-file locking.
 *
 * Protocol (text lines, \n terminated):
 *   CACHE <path>                              → OK | ERR <msg>
 *   INSERT <path> <line> <col> <len>\n<bytes> → OK <id> | ERR
 *   DELETE <path> <sl> <sc> <el> <ec>         → OK <id> | ERR
 *   REPLACE <path> <sl> <sc> <el> <ec> <len>\n<bytes> → OK <id>
 *   DEP <id> <dep_id>                         → OK | ERR
 *   SUBMIT <id>                               → OK | ERR
 *   FLUSH                                     → FLUSHED <n>
 *   SYNC                                      → SYNCED 0 | ERR
 *   GET <path>                                → DATA <len>\n<bytes> | ERR
 *   INFO <path>                               → INFO <lines> <bytes> | ERR
 *   QUIT                                      → BYE (close conn)
 *   KILL                                      → BYE (shutdown daemon)
 *
 * Build: gcc -std=c11 -Wall -O2 -o ped ped.c pe.o -pthread
 * Usage: ./ped [socket_path] [project_root]
 */

#define _GNU_SOURCE
#include "pe.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdarg.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/poll.h>
#include <sys/stat.h>

/* ── Constants ─────────────────────────────────────────── */

#define MAX_CLIENTS   64
#define MAX_EDITS   4096
#define BUF_SIZE   65536
#define PATH_MAXLEN 4096

/* ── Client state ──────────────────────────────────────── */

typedef enum { S_CMD, S_CONTENT } st_t;

typedef struct {
    int   fd;
    st_t  state;
    char *buf;
    size_t buf_len;

    /* Content-read mode */
    char  *content;
    size_t content_off;
    size_t content_total;

    /* Parsed command fields */
    char   cmd[32];
    char   path[PATH_MAXLEN];
    size_t line, col, eline, ecol;
    int    edit_id, dep_id;
    size_t clen;
} client_t;

/* ── Edit table ────────────────────────────────────────── */

typedef struct {
    pe_edit_t *edit;
    int        id;
    bool       submitted;
} eslot_t;

/* ── Globals ───────────────────────────────────────────── */

static pe_t     *g_pe;
static client_t *g_clients[MAX_CLIENTS];
static size_t    g_nclients;
static eslot_t   g_edits[MAX_EDITS];
static int       g_next_id = 1;
static int       g_listen_fd = -1;
static sig_atomic_t volatile g_running = 1;

/* ── Signal ────────────────────────────────────────────── */

static void on_signal(int s) { (void)s; g_running = 0; }

/* ── Client mgmt ───────────────────────────────────────── */

static client_t *client_new(int fd) {
    client_t *c = calloc(1, sizeof(client_t));
    if (!c) return NULL;
    c->fd  = fd;
    c->buf = malloc(BUF_SIZE);
    if (!c->buf) { free(c); return NULL; }
    return c;
}

static void client_free(client_t *c) {
    if (!c) return;
    if (c->fd >= 0) close(c->fd);
    free(c->buf);
    free(c->content);
    free(c);
}

/* ── Response helpers ──────────────────────────────────── */

static void respond(client_t *c, const char *msg) {
    size_t len = strlen(msg);
    for (size_t off = 0; off < len; ) {
        ssize_t n = write(c->fd, msg + off, len - off);
        if (n <= 0) break;
        off += (size_t)n;
    }
}

__attribute__((format(printf, 2, 3)))
static void respondf(client_t *c, const char *fmt, ...) {
    char tmp[4096];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(tmp, sizeof(tmp), fmt, ap);
    va_end(ap);
    if (n > 0) respond(c, tmp);
}

static void respond_raw(client_t *c, const char *data, size_t len) {
    for (size_t off = 0; off < len; ) {
        ssize_t n = write(c->fd, data + off, len - off);
        if (n <= 0) break;
        off += (size_t)n;
    }
}

/* ── Command parsing ───────────────────────────────────── */

static int parse_cmd(char *line, client_t *c) {
    c->path[0] = '\0';
    c->line = c->col = c->eline = c->ecol = 0;
    c->clen = c->edit_id = c->dep_id = 0;

    char *tok = strtok(line, " \t");
    if (!tok) return -1;
    strncpy(c->cmd, tok, sizeof(c->cmd) - 1);
    c->cmd[sizeof(c->cmd) - 1] = '\0';

    #define NEXT()  (tok = strtok(NULL, " \t"))
    #define NEED()  do { if (!NEXT()) return -1; } while(0)

    if (!strcmp(c->cmd, "CACHE") || !strcmp(c->cmd, "GET") ||
        !strcmp(c->cmd, "INFO") || !strcmp(c->cmd, "UNDO") ||
        !strcmp(c->cmd, "BRANCH") || !strcmp(c->cmd, "SWITCH") ||
        !strcmp(c->cmd, "DELETE_BRANCH")) {
        NEED(); strncpy(c->path, tok, PATH_MAXLEN - 1);
        return 0;
    }
    if (!strcmp(c->cmd, "MERGE")) {
        NEED(); strncpy(c->path, tok, PATH_MAXLEN - 1);
        return 0;
    }
    if (!strcmp(c->cmd, "DIFF")) {
        NEED(); strncpy(c->path, tok, PATH_MAXLEN - 1);
        if (NEXT()) c->line = strtoul(tok, NULL, 10);
        return 0;
    }
    if (!strcmp(c->cmd, "FLUSH") || !strcmp(c->cmd, "SYNC") ||
        !strcmp(c->cmd, "QUIT")  || !strcmp(c->cmd, "KILL") ||
        !strcmp(c->cmd, "BEGIN") || !strcmp(c->cmd, "COMMIT") ||
        !strcmp(c->cmd, "ROLLBACK") || !strcmp(c->cmd, "BRANCHES"))
        return 0;

    if (!strcmp(c->cmd, "INSERT") || !strcmp(c->cmd, "REPLACE")) {
        NEED(); strncpy(c->path, tok, PATH_MAXLEN - 1);
        NEED(); c->line = strtoul(tok, NULL, 10);
        NEED(); c->col  = strtoul(tok, NULL, 10);
        if (!strcmp(c->cmd, "REPLACE")) {
            NEED(); c->eline = strtoul(tok, NULL, 10);
            NEED(); c->ecol  = strtoul(tok, NULL, 10);
        }
        NEED(); c->clen = strtoul(tok, NULL, 10);
        return 0;
    }
    if (!strcmp(c->cmd, "DELETE")) {
        NEED(); strncpy(c->path, tok, PATH_MAXLEN - 1);
        NEED(); c->line  = strtoul(tok, NULL, 10);
        NEED(); c->col   = strtoul(tok, NULL, 10);
        NEED(); c->eline = strtoul(tok, NULL, 10);
        NEED(); c->ecol  = strtoul(tok, NULL, 10);
        return 0;
    }
    if (!strcmp(c->cmd, "DEP")) {
        NEED(); c->edit_id = atoi(tok);
        NEED(); c->dep_id  = atoi(tok);
        return 0;
    }
    if (!strcmp(c->cmd, "SUBMIT")) {
        NEED(); c->edit_id = atoi(tok);
        return 0;
    }

    #undef NEED
    #undef NEXT
    return -1;
}

/* ── Command execution ─────────────────────────────────── */

static void exec_cmd(client_t *c) {
    if (!strcmp(c->cmd, "CACHE")) {
        respond(c, pe_cache_file(g_pe, c->path) == 0 ? "OK\n" : "ERR\n");
        return;
    }
    if (!strcmp(c->cmd, "FLUSH")) {
        size_t nf = pe_flush(g_pe);
        respondf(c, "FLUSHED %zu\n", nf);
        return;
    }
    if (!strcmp(c->cmd, "SYNC")) {
        respond(c, pe_sync(g_pe) == 0 ? "SYNCED 0\n" : "ERR sync\n");
        return;
    }
    if (!strcmp(c->cmd, "QUIT")) {
        respond(c, "BYE\n");
        c->fd = -2;  /* signal caller to disconnect */
        return;
    }
    if (!strcmp(c->cmd, "KILL")) {
        respond(c, "BYE\n");
        g_running = 0;
        return;
    }
    if (!strcmp(c->cmd, "GET")) {
        size_t sz;
        const char *data = pe_file_data(g_pe, c->path, &sz);
        if (!data) { respondf(c, "ERR %s\n", c->path); return; }
        respondf(c, "DATA %zu\n", sz);
        respond_raw(c, data, sz);
        return;
    }
    if (!strcmp(c->cmd, "INFO")) {
        size_t sz;
        if (!pe_file_data(g_pe, c->path, &sz))
            { respondf(c, "ERR %s\n", c->path); return; }
        respondf(c, "INFO %zu %zu\n", pe_file_lines(g_pe, c->path), sz);
        return;
    }
    if (!strcmp(c->cmd, "DIFF")) {
        size_t ctx = (c->line > 0) ? c->line : 3;
        char *d = pe_diff(g_pe, c->path, ctx);
        if (!d) { respondf(c, "ERR %s\n", c->path); return; }
        if (d[0]) { respondf(c, "DIFF %zu\n", strlen(d)); respond_raw(c, d, strlen(d)); }
        else respond(c, "DIFF 0\n");
        free(d);
        return;
    }
    if (!strcmp(c->cmd, "UNDO")) {
        int rc = pe_undo(g_pe, c->path);
        respond(c, rc == 0 ? "OK\n" : "ERR undo\n");
        return;
    }
    if (!strcmp(c->cmd, "BEGIN")) {
        respond(c, pe_txn_begin(g_pe) == 0 ? "OK\n" : "ERR txn\n");
        return;
    }
    if (!strcmp(c->cmd, "COMMIT")) {
        size_t nf = pe_txn_commit(g_pe);
        respondf(c, "COMMITTED %zu\n", nf);
        return;
    }
    if (!strcmp(c->cmd, "ROLLBACK")) {
        pe_txn_rollback(g_pe);
        respond(c, "OK\n");
        return;
    }
    if (!strcmp(c->cmd, "BRANCH")) {
        int rc = pe_branch_create(g_pe, c->path);
        respond(c, rc == 0 ? "OK\n" : "ERR exists\n");
        return;
    }
    if (!strcmp(c->cmd, "SWITCH")) {
        int rc = pe_branch_switch(g_pe, c->path);
        respond(c, rc == 0 ? "OK\n" : "ERR not found\n");
        return;
    }
    if (!strcmp(c->cmd, "MERGE")) {
        /* MERGE <from_branch> <to_branch> */
        /* c->path = from, c->cmd reused for to */
        char to_branch[PATH_MAXLEN];
        char *t = strtok(NULL, " \t\r\n");
        if (t) strncpy(to_branch, t, sizeof(to_branch)-1);
        else { respond(c, "ERR usage: MERGE from to\n"); return; }
        int rc = pe_branch_merge(g_pe, c->path, to_branch);
        if (rc == 0) respond(c, "OK\n");
        else if (rc == 1) respond(c, "CONFLICTS\n");
        else respond(c, "ERR\n");
        return;
    }
    if (!strcmp(c->cmd, "BRANCHES")) {
        char **names;
        size_t n = pe_branch_list(g_pe, &names);
        respondf(c, "BRANCHES %zu%s\n", n, n > 0 ? "" : "");
        for (size_t i = 0; i < n; i++) {
            respondf(c, "%s%s\n", names[i],
                     strcmp(names[i], pe_branch_current(g_pe)) == 0 ? " *" : "");
            free(names[i]);
        }
        free(names);
        return;
    }
    if (!strcmp(c->cmd, "DELETE_BRANCH")) {
        int rc = pe_branch_delete(g_pe, c->path);
        respond(c, rc == 0 ? "OK\n" : "ERR\n");
        return;
    }
    if (!strcmp(c->cmd, "INSERT") || !strcmp(c->cmd, "REPLACE") ||
        !strcmp(c->cmd, "DELETE")) {

        if (g_next_id >= MAX_EDITS) { respond(c, "ERR full\n"); return; }

        pe_op_t op = !strcmp(c->cmd, "INSERT") ? PE_OP_INSERT
                   : !strcmp(c->cmd, "DELETE") ? PE_OP_DELETE
                   : PE_OP_REPLACE;

        pe_edit_t *e = pe_edit_create(g_pe, c->path, op,
            (pe_pos_t){c->line, c->col},
            (pe_pos_t){c->eline, c->ecol},
            c->content, c->content_total);
        if (!e) { respond(c, "ERR create\n"); return; }

        int id = g_next_id++;
        g_edits[id].edit      = e;
        g_edits[id].id        = id;
        g_edits[id].submitted = false;
        respondf(c, "OK %d\n", id);
        return;
    }
    if (!strcmp(c->cmd, "DEP")) {
        if (c->edit_id <= 0 || c->edit_id >= g_next_id ||
            c->dep_id  <= 0 || c->dep_id  >= g_next_id ||
            !g_edits[c->edit_id].edit || !g_edits[c->dep_id].edit)
            { respond(c, "ERR bad id\n"); return; }
        if (pe_edit_depend(g_edits[c->edit_id].edit,
                           g_edits[c->dep_id].edit) != 0)
            { respond(c, "ERR dep\n"); return; }
        respond(c, "OK\n");
        return;
    }
    if (!strcmp(c->cmd, "SUBMIT")) {
        if (c->edit_id <= 0 || c->edit_id >= g_next_id ||
            !g_edits[c->edit_id].edit)
            { respond(c, "ERR bad id\n"); return; }
        if (g_edits[c->edit_id].submitted)
            { respond(c, "ERR dup submit\n"); return; }
        if (pe_edit_submit(g_pe, g_edits[c->edit_id].edit) != 0)
            { respond(c, "ERR submit\n"); return; }
        g_edits[c->edit_id].submitted = true;
        respond(c, "OK\n");
        return;
    }

    respondf(c, "ERR unknown: %s\n", c->cmd);
}

/* ── Process one client's readable data ────────────────── */
/* Returns true if client should stay connected. */

static bool process_client(client_t *c) {
    /* Grow buffer if tight */
    if (c->buf_len > BUF_SIZE - 4096) {
        size_t nc = c->buf_len + BUF_SIZE;
        char *nb = realloc(c->buf, nc);
        if (!nb) return false;
        c->buf = nb;
    }

    ssize_t nr = read(c->fd, c->buf + c->buf_len, BUF_SIZE - 1);
    if (nr <= 0) return false;
    c->buf_len += (size_t)nr;

    size_t pos = 0;

    while (pos < c->buf_len) {
        if (c->state == S_CMD) {
            char *nl = memchr(c->buf + pos, '\n', c->buf_len - pos);
            if (!nl) break;

            *nl = '\0';
            char *line = c->buf + pos;
            pos = (size_t)((nl + 1) - c->buf);

            if (line[0] == '\0') continue;  /* skip blank */

            if (parse_cmd(line, c) != 0) {
                respondf(c, "ERR parse\n");
                continue;
            }

            /* Commands with payload */
            if (c->clen > 0) {
                c->state         = S_CONTENT;
                c->content_total = c->clen;
                c->content_off   = 0;
                free(c->content);
                c->content = malloc(c->clen + 1);
                if (!c->content) { respond(c, "ERR oom\n"); return false; }
                /* fall through to content-read */
            } else {
                exec_cmd(c);
                if (c->fd == -2) { close(c->fd); c->fd = -1; return false; }
                if (!g_running)   return true;  /* KILL — let main loop exit */
                continue;
            }
        }

        /* c->state == S_CONTENT */
        {
            size_t avail = c->buf_len - pos;
            size_t need  = c->content_total - c->content_off;
            size_t take  = avail < need ? avail : need;

            memcpy(c->content + c->content_off, c->buf + pos, take);
            c->content_off += take;
            pos            += take;

            if (c->content_off >= c->content_total) {
                c->content[c->content_total] = '\0';
                exec_cmd(c);
                free(c->content);
                c->content = NULL;
                c->state   = S_CMD;
                c->clen    = 0;
                if (c->fd == -2) { close(c->fd); c->fd = -1; return false; }
                if (!g_running)   return true;
            } else {
                break;  /* need more data */
            }
        }
    }

    /* Compact un-consumed tail */
    if (pos > 0 && pos < c->buf_len)
        memmove(c->buf, c->buf + pos, c->buf_len - pos);
    if (pos <= c->buf_len)
        c->buf_len -= pos;

    return true;
}

/* ── Main ──────────────────────────────────────────────── */

int main(int argc, char **argv) {
    const char *sock = (argc > 1) ? argv[1] : "/tmp/pe.sock";
    const char *root = (argc > 2) ? argv[2] : ".";

    g_pe = pe_create(root, 0);
    if (!g_pe) { fprintf(stderr, "ped: pe_create failed\n"); return 1; }

    signal(SIGINT,  on_signal);
    signal(SIGTERM, on_signal);
    signal(SIGPIPE, SIG_IGN);

    g_listen_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (g_listen_fd < 0) { perror("socket"); pe_destroy(g_pe); return 1; }

    {
        int one = 1;
        setsockopt(g_listen_fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    }

    struct sockaddr_un addr = {.sun_family = AF_UNIX};
    strncpy(addr.sun_path, sock, sizeof(addr.sun_path) - 1);
    unlink(sock);

    if (bind(g_listen_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind"); close(g_listen_fd); pe_destroy(g_pe); return 1;
    }
    chmod(sock, 0600);

    if (listen(g_listen_fd, 16) < 0) {
        perror("listen"); unlink(sock); close(g_listen_fd);
        pe_destroy(g_pe); return 1;
    }

    printf("ped: listening on %s (root: %s)\n", sock, root);

    struct pollfd fds[MAX_CLIENTS + 1];

    while (g_running) {
        int nfds = 0;
        fds[nfds].fd = g_listen_fd;
        fds[nfds].events = POLLIN;
        nfds++;

        for (size_t i = 0; i < g_nclients; i++) {
            fds[nfds].fd     = g_clients[i]->fd;
            fds[nfds].events = POLLIN;
            nfds++;
        }

        if (poll(fds, nfds, 500) < 0) {
            if (errno == EINTR) continue;
            perror("poll"); break;
        }

        /* Accept */
        if (fds[0].revents & POLLIN) {
            int cfd = accept(g_listen_fd, NULL, NULL);
            if (cfd >= 0) {
                if (g_nclients < MAX_CLIENTS) {
                    client_t *nc = client_new(cfd);
                    if (nc) {
                        g_clients[g_nclients++] = nc;
                        respond(nc, "READY\n");
                    } else close(cfd);
                } else {
                    close(cfd);
                }
            }
        }

        /* Process clients (reverse order so removal is safe) */
        for (size_t i = g_nclients; i > 0; ) {
            i--;
            if (fds[i + 1].revents & (POLLIN | POLLHUP | POLLERR)) {
                if (!process_client(g_clients[i])) {
                    client_free(g_clients[i]);
                    g_clients[i] = g_clients[--g_nclients];
                }
            }
        }
    }

    printf("ped: shutting down...\n");
    for (size_t i = 0; i < g_nclients; i++)
        client_free(g_clients[i]);
    close(g_listen_fd);
    unlink(sock);
    pe_destroy(g_pe);
    printf("ped: done\n");
    return 0;
}
