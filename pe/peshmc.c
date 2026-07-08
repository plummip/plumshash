/*
 * peshmc.c — Project Editor Shared-Memory Client
 *
 * Reads edit commands from stdin (same text protocol as ped),
 * writes them into the shared-memory ring buffer,
 * waits for the daemon's response, prints to stdout.
 *
 * Build:  gcc -std=c11 -Wall -O2 -o peshmc peshmc.c -pthread
 * Usage:  echo "CACHE main.c" | ./peshmc
 *         ./peshmc < commands.txt
 */

#define _GNU_SOURCE
#include "pe_shm.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>

#ifndef PE_SHM_PATH
#define PE_SHM_PATH "/data/data/com.termux/files/home/tmp/pe_shm"
#endif

/* ── Ring-buffer write helpers ─────────────────────────── */

static inline size_t rb_off(uint64_t c) {
    return (size_t)(c & (PE_SHM_RING_SIZE - 1));
}

static void rb_write(volatile pe_shm_t *s, size_t off,
                     const void *src, size_t len) {
    const uint8_t *p = (const uint8_t *)src;
    size_t first = PE_SHM_RING_SIZE - off;
    if (len <= first) {
        memcpy((void *)(s->cmd_buf + off), p, len);
    } else {
        memcpy((void *)(s->cmd_buf + off), p, first);
        memcpy((void *)s->cmd_buf, p + first, len - first);
    }
}

/* ── Globals ───────────────────────────────────────────── */

static pe_shm_t *g_shm;
static int       g_shm_fd = -1;
static int       g_client_id = 0;

/* ── Client registration ───────────────────────────────── */

static int client_register(void) {
    pthread_mutex_lock(&g_shm->reg_lock);

    for (int i = 1; i <= PE_SHM_MAX_CLIENTS; i++) {
        if (!g_shm->slots[i].in_use) {
            g_shm->slots[i].in_use = 1;
            g_shm->slots[i].pid    = getpid();
            g_shm->slots[i].has_resp = 0;
            g_client_id = i;
            pthread_mutex_unlock(&g_shm->reg_lock);
            return 0;
        }
    }

    pthread_mutex_unlock(&g_shm->reg_lock);
    return -1;
}

static void client_unregister(void) {
    if (g_client_id > 0) {
        g_shm->slots[g_client_id].in_use = 0;
        g_client_id = 0;
    }
}

/* ── Send command to daemon ────────────────────────────── */

static int send_cmd(const pe_cmd_hdr_t *hdr,
                    const char *path, const char *content) {
    size_t total = sizeof(pe_cmd_hdr_t) + hdr->path_len + hdr->content_len;
    /* Align to 8 bytes */
    size_t padded = (total + 7) & ~7UL;
    /* Plus 4-byte length prefix */
    size_t entry_size = padded + 4;

    pthread_mutex_lock(&g_shm->cmd_lock);

    /* Check available space */
    size_t used = (size_t)(g_shm->cmd_tail - g_shm->cmd_head);
    if (used + entry_size > PE_SHM_RING_SIZE) {
        pthread_mutex_unlock(&g_shm->cmd_lock);
        fprintf(stderr, "peshmc: ring buffer full\n");
        return -1;
    }

    size_t off = rb_off(g_shm->cmd_tail);
    size_t space_to_end = PE_SHM_RING_SIZE - off;

    /* If not enough room at end for entry, insert padding and wrap */
    if (space_to_end < entry_size) {
        /* Write a zero-length padding entry to consume remaining space */
        if (space_to_end >= 4) {
            uint32_t pad = 0;
            memcpy((void *)(g_shm->cmd_buf + off), &pad, 4);
            g_shm->cmd_tail += space_to_end;
        }
        off = 0;
        used = (size_t)(g_shm->cmd_tail - g_shm->cmd_head);
        if (used + entry_size > PE_SHM_RING_SIZE) {
            pthread_mutex_unlock(&g_shm->cmd_lock);
            fprintf(stderr, "peshmc: ring full after wrap\n");
            return -1;
        }
    }

    /* Write length prefix */
    uint32_t prefix = (uint32_t)entry_size;
    memcpy((void *)(g_shm->cmd_buf + off), &prefix, 4);
    off = (off + 4) & (PE_SHM_RING_SIZE - 1);

    /* Write header */
    pe_cmd_hdr_t whdr = *hdr;
    whdr.total_len = (uint32_t)entry_size;
    rb_write(g_shm, off, &whdr, sizeof(whdr));
    off = rb_off(g_shm->cmd_tail + 4 + sizeof(whdr));

    /* Write path */
    if (hdr->path_len > 0) {
        rb_write(g_shm, off, path, hdr->path_len);
        off = rb_off(g_shm->cmd_tail + 4 + sizeof(whdr) + hdr->path_len);
    }

    /* Write content */
    if (hdr->content_len > 0) {
        rb_write(g_shm, off, content, hdr->content_len);
        off = rb_off(g_shm->cmd_tail + 4 + sizeof(whdr) +
                     hdr->path_len + hdr->content_len);
    }

    /* Pad to 8 bytes */
    size_t written = 4 + sizeof(whdr) + hdr->path_len + hdr->content_len;
    while (written < padded) {
        g_shm->cmd_buf[off] = 0;
        off = (off + 1) & (PE_SHM_RING_SIZE - 1);
        written++;
    }

    g_shm->cmd_tail += entry_size;

    /* Wake daemon */
    pthread_cond_signal(&g_shm->cmd_cond);
    pthread_mutex_unlock(&g_shm->cmd_lock);

    return 0;
}

/* ── Wait for response ─────────────────────────────────── */

static void wait_resp(void) {
    pe_resp_slot_t *r = &g_shm->slots[g_client_id];

    pthread_mutex_lock(&r->lock);
    while (!r->has_resp)
        pthread_cond_wait(&r->cond, &r->lock);

    if (r->status == 0) {
        /* OK — print data */
        fwrite(r->data, 1, r->data_len, stdout);
        fflush(stdout);
    } else {
        /* Error */
        fprintf(stderr, "peshmc: ERR: %.*s\n",
                (int)r->data_len, r->data);
    }

    r->has_resp = 0;
    pthread_mutex_unlock(&r->lock);
}

/* ── Command-line parsing (same text protocol as ped) ──── */

static int parse_and_send(char *line) {
    char cmd[32], path[4096], path2[4096];
    size_t line_n, col_n, eline, ecol, clen;
    int eid, did;

    path[0] = '\0'; path2[0] = '\0';
    line_n = col_n = eline = ecol = clen = 0;
    eid = did = 0;

    char *tok = strtok(line, " \t\r\n");
    if (!tok) return 0;  /* empty line */
    strncpy(cmd, tok, sizeof(cmd) - 1);
    cmd[sizeof(cmd) - 1] = '\0';

    #define NEXT() (tok = strtok(NULL, " \t\r\n"))

    if (!strcmp(cmd, "CACHE") || !strcmp(cmd, "GET") ||
        !strcmp(cmd, "INFO") || !strcmp(cmd, "UNDO") ||
        !strcmp(cmd, "BRANCH") || !strcmp(cmd, "SWITCH") ||
        !strcmp(cmd, "DELETE_BRANCH")) {
        if (!NEXT()) return -1;
        strncpy(path, tok, sizeof(path) - 1);
    } else if (!strcmp(cmd, "DIFF")) {
        if (!NEXT()) return -1;
        strncpy(path, tok, sizeof(path) - 1);
        if (NEXT()) line_n = strtoul(tok, NULL, 10);  /* optional context */
    } else if (!strcmp(cmd, "MERGE")) {
        if (!NEXT()) return -1;
        strncpy(path, tok, sizeof(path) - 1);   /* from */
        if (!NEXT()) return -1;
        strncpy(path2, tok, sizeof(path2) - 1);  /* to */
    } else if (!strcmp(cmd, "FLUSH") || !strcmp(cmd, "SYNC") ||
               !strcmp(cmd, "QUIT")  || !strcmp(cmd, "KILL") ||
               !strcmp(cmd, "BRANCHES") || !strcmp(cmd, "BEGIN") ||
               !strcmp(cmd, "COMMIT") || !strcmp(cmd, "ROLLBACK")) {
        /* no args */
    } else if (!strcmp(cmd, "INSERT") || !strcmp(cmd, "REPLACE")) {
        if (!NEXT()) return -1;
        strncpy(path, tok, sizeof(path) - 1);
        if (!NEXT()) return -1;
        line_n = strtoul(tok, NULL, 10);
        if (!NEXT()) return -1;
        col_n  = strtoul(tok, NULL, 10);
        if (!strcmp(cmd, "REPLACE")) {
            if (!NEXT()) return -1;
            eline = strtoul(tok, NULL, 10);
            if (!NEXT()) return -1;
            ecol  = strtoul(tok, NULL, 10);
        }
        if (!NEXT()) return -1;
        clen = strtoul(tok, NULL, 10);
    } else if (!strcmp(cmd, "DELETE")) {
        if (!NEXT()) return -1;
        strncpy(path, tok, sizeof(path) - 1);
        if (!NEXT()) return -1; line_n = strtoul(tok, NULL, 10);
        if (!NEXT()) return -1; col_n  = strtoul(tok, NULL, 10);
        if (!NEXT()) return -1; eline  = strtoul(tok, NULL, 10);
        if (!NEXT()) return -1; ecol   = strtoul(tok, NULL, 10);
    } else if (!strcmp(cmd, "DEP")) {
        if (!NEXT()) return -1; eid = atoi(tok);
        if (!NEXT()) return -1; did = atoi(tok);
    } else if (!strcmp(cmd, "SUBMIT")) {
        if (!NEXT()) return -1;
        eid = atoi(tok);
    } else {
        fprintf(stderr, "peshmc: unknown command: %s\n", cmd);
        return -1;
    }

    #undef NEXT

    /* Map string command to cmd_type */
    int ctype;
    if (!strcmp(cmd, "CACHE"))        ctype = PE_CMD_CACHE;
    else if (!strcmp(cmd, "INSERT"))  ctype = PE_CMD_INSERT;
    else if (!strcmp(cmd, "DELETE"))  ctype = PE_CMD_DELETE;
    else if (!strcmp(cmd, "REPLACE")) ctype = PE_CMD_REPLACE;
    else if (!strcmp(cmd, "DEP"))     ctype = PE_CMD_DEP;
    else if (!strcmp(cmd, "SUBMIT"))  ctype = PE_CMD_SUBMIT;
    else if (!strcmp(cmd, "FLUSH"))   ctype = PE_CMD_FLUSH;
    else if (!strcmp(cmd, "SYNC"))    ctype = PE_CMD_SYNC;
    else if (!strcmp(cmd, "GET"))     ctype = PE_CMD_GET;
    else if (!strcmp(cmd, "INFO"))    ctype = PE_CMD_INFO;
    else if (!strcmp(cmd, "QUIT"))    ctype = PE_CMD_QUIT;
    else if (!strcmp(cmd, "KILL"))    ctype = PE_CMD_KILL;
    else if (!strcmp(cmd, "UNDO"))    ctype = PE_CMD_UNDO;
    else if (!strcmp(cmd, "DIFF"))    ctype = PE_CMD_DIFF;
    else if (!strcmp(cmd, "BRANCH"))  ctype = PE_CMD_BRANCH;
    else if (!strcmp(cmd, "SWITCH"))  ctype = PE_CMD_SWITCH;
    else if (!strcmp(cmd, "MERGE"))   ctype = PE_CMD_MERGE;
    else if (!strcmp(cmd, "BRANCHES")) ctype = PE_CMD_BRANCHES;
    else if (!strcmp(cmd, "DELETE_BRANCH")) ctype = PE_CMD_DELBR;
    else if (!strcmp(cmd, "BEGIN"))   ctype = PE_CMD_BEGIN;
    else if (!strcmp(cmd, "COMMIT"))  ctype = PE_CMD_COMMIT;
    else if (!strcmp(cmd, "ROLLBACK")) ctype = PE_CMD_ROLLBACK;
    else return -1;

    /* Read content from stdin if needed */
    char *content = NULL;
    if (clen > 0) {
        content = malloc(clen + 1);
        if (!content) return -1;
        size_t off = 0;
        while (off < clen) {
            ssize_t n = read(STDIN_FILENO, content + off, clen - off);
            if (n <= 0) break;
            off += (size_t)n;
        }
        content[off] = '\0';
        if (off < clen) { free(content); return -1; }
    }

    /* Build header */
    pe_cmd_hdr_t hdr = {
        .total_len   = 0,  /* filled by send_cmd */
        .cmd_type    = (uint32_t)ctype,
        .client_id   = (uint32_t)g_client_id,
        .edit_id     = (uint32_t)eid,
        .dep_id      = (uint32_t)did,
        .path_len    = (uint32_t)strlen(path),
        .content_len = (uint32_t)((ctype == PE_CMD_MERGE) ? strlen(path2) : clen),
        .start_line  = line_n,
        .start_col   = col_n,
        .end_line    = eline,
        .end_col     = ecol,
    };

    const char *content_data = content ? content : "";
    if (ctype == PE_CMD_MERGE) content_data = path2;
    int rc = send_cmd(&hdr, path, content_data);
    free(content);
    if (rc != 0) return -1;

    /* Wait for daemon response */
    wait_resp();

    /* QUIT / KILL terminate the client */
    if (ctype == PE_CMD_QUIT || ctype == PE_CMD_KILL)
        return 2;  /* signal to exit */

    return 0;
}

/* ── Main ──────────────────────────────────────────────── */

int main(void) {
    /* Ignore SIGPIPE */
    signal(SIGPIPE, SIG_IGN);

    /* Open + mmap the shared memory file */
    g_shm_fd = open(PE_SHM_PATH, O_RDWR);
    if (g_shm_fd < 0) {
        perror("open shm — is daemon running?");
        return 1;
    }

    g_shm = mmap(NULL, sizeof(pe_shm_t), PROT_READ | PROT_WRITE,
                 MAP_SHARED, g_shm_fd, 0);
    close(g_shm_fd);
    g_shm_fd = -1;

    if (g_shm == MAP_FAILED) {
        perror("mmap");
        return 1;
    }

    /* Verify magic */
    if (g_shm->magic != PE_SHM_MAGIC) {
        fprintf(stderr, "peshmc: bad magic (corrupt shm?)\n");
        munmap(g_shm, sizeof(pe_shm_t));
        return 1;
    }

    /* Register */
    if (client_register() != 0) {
        fprintf(stderr, "peshmc: no free client slots\n");
        munmap(g_shm, sizeof(pe_shm_t));
        return 1;
    }

    /* Read commands from stdin */
    char *line = NULL;
    size_t linecap = 0;
    ssize_t linelen;

    while ((linelen = getline(&line, &linecap, stdin)) > 0) {
        /* Strip trailing newline */
        if (linelen > 0 && line[linelen - 1] == '\n')
            line[--linelen] = '\0';
        if (linelen > 0 && line[linelen - 1] == '\r')
            line[--linelen] = '\0';

        int rc = parse_and_send(line);
        if (rc == 2) break;  /* QUIT/KILL */
        if (rc < 0) {
            fprintf(stderr, "peshmc: parse error at: %s\n", line);
        }
    }

    free(line);
    client_unregister();
    munmap(g_shm, sizeof(pe_shm_t));
    return 0;
}
