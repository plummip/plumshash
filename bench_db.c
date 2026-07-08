/*
 * bench_db.c — Full-text database benchmark: SQLite FTS5 vs ffdb vs fuzzydb vs fastdb.
 *
 * Compares insert throughput, fuzzy search latency, and result correctness.
 * Uses deterministic PRNG for reproducible test data.
 *
 * Compile:
 *   gcc -O3 -march=armv8-a -Wall -Wextra -Werror \
 *       -o bench_db bench_db.c -lsqlite3 -I.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <ctype.h>
#include <time.h>
#include <sys/stat.h>
#include <unistd.h>
#include <sqlite3.h>

/* ── xorshift64 PRNG ── */
static uint64_t xs64_state;
static uint64_t xs64(void) {
    uint64_t x = xs64_state;
    x ^= x << 13; x ^= x >> 7; x ^= x << 17;
    return xs64_state = x;
}

/* ── Realistic city names pool ── */
static const char *cities[] = {
    "Amsterdam","Rotterdam","Utrecht","Eindhoven","Groningen",
    "London","Manchester","Birmingham","Liverpool","Bristol",
    "Paris","Marseille","Lyon","Toulouse","Bordeaux",
    "Berlin","Hamburg","Munich","Cologne","Frankfurt",
    "Madrid","Barcelona","Valencia","Seville","Bilbao",
    "Rome","Milan","Naples","Turin","Florence",
    "New York","Los Angeles","Chicago","Houston","Phoenix",
    "Tokyo","Osaka","Kyoto","Nagoya","Sapporo",
    "Sydney","Melbourne","Brisbane","Perth","Adelaide",
    "Moscow","Saint Petersburg","Novosibirsk","Yekaterinburg","Kazan",
    "Beijing","Shanghai","Shenzhen","Guangzhou","Chengdu",
    "Mumbai","Delhi","Bangalore","Hyderabad","Chennai",
    "Cairo","Lagos","Nairobi","Cape Town","Johannesburg",
    "Toronto","Vancouver","Montreal","Ottawa","Calgary",
    "Mexico City","Buenos Aires","Sao Paulo","Lima","Santiago",
    "Stockholm","Oslo","Copenhagen","Helsinki","Reykjavik",
    "Warsaw","Prague","Vienna","Budapest","Bucharest",
    "Istanbul","Ankara","Tehran","Baghdad","Riyadh",
    "Seoul","Bangkok","Jakarta","Manila","Hanoi"
};
#define N_CITIES 85

/* Generate a city-like string (either a real city or a typo'd one) */
static void gen_city(char *buf, int maxlen) {
    if (xs64() % 4 == 0) {
        /* Generate a random string of 5-15 chars */
        int len = 5 + (int)(xs64() % 11);
        for (int i = 0; i < len && i < maxlen-1; i++)
            buf[i] = 'a' + (int)(xs64() % 26);
        buf[len < maxlen-1 ? len : maxlen-1] = 0;
    } else {
        /* Pick a real city */
        const char *c = cities[xs64() % N_CITIES];
        int cl = strlen(c);
        if (cl >= maxlen) cl = maxlen - 1;
        memcpy(buf, c, cl);
        buf[cl] = 0;
    }
}

/* Mutate: apply k random edits to a string */
static int mutate(const char *src, int slen, char *dst, int k) {
    memcpy(dst, src, slen);
    int dlen = slen;
    for (int e = 0; e < k; e++) {
        if (dlen <= 1) { dst[0] = 'a' + (int)(xs64()%26); break; }
        if (dlen >= 60) break;
        int op = (int)(xs64() % 3);
        int pos = (int)(xs64() % dlen);
        switch (op) {
        case 0: dst[pos] = 'a' + (int)(xs64() % 26); break;
        case 1: if (pos < dlen-1) { memmove(dst+pos, dst+pos+1, dlen-pos-1); dlen--; } break;
        case 2: memmove(dst+pos+1, dst+pos, dlen-pos); dst[pos] = 'a' + (int)(xs64()%26); dlen++; break;
        }
    }
    dst[dlen] = 0;
    return dlen;
}

/* ── Timing ── */
static double now_sec(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

/* ── Shell out to ffdb/fuzzydb/fastdb ── */
static int shell_cmd(const char *cmd) {
    return system(cmd);
}

/* ── SQLite FTS5 wrapper ── */
static sqlite3 *fts5_db = NULL;

static int fts5_open(const char *path) {
    unlink(path);
    int rc = sqlite3_open(path, &fts5_db);
    if (rc) { fprintf(stderr, "sqlite3_open: %s\n", sqlite3_errmsg(fts5_db)); return -1; }
    rc = sqlite3_exec(fts5_db,
        "CREATE VIRTUAL TABLE docs USING fts5(content);", NULL, NULL, NULL);
    if (rc) { fprintf(stderr, "fts5 create: %s\n", sqlite3_errmsg(fts5_db)); return -1; }
    return 0;
}

static int fts5_insert(const char *text) {
    char *esc = sqlite3_mprintf("%q", text);
    char *sql = sqlite3_mprintf("INSERT INTO docs VALUES('%s');", esc);
    char *err = NULL;
    int rc = sqlite3_exec(fts5_db, sql, NULL, NULL, &err);
    sqlite3_free(sql); sqlite3_free(esc);
    if (rc) { fprintf(stderr, "fts5 insert: %s\n", err); sqlite3_free(err); return -1; }
    return 0;
}

static int fts5_search(const char *query, int max_results, char *out, int outsz) {
    /* FTS5 doesn't do fuzzy/typo search natively. We use prefix + LIKE.
     * For fair comparison: use 'query*' prefix search which finds
     * documents starting with the query. Not edit-distance based but
     * it's what FTS5 offers. */
    char *esc = sqlite3_mprintf("%q", query);
    char *sql = sqlite3_mprintf(
        "SELECT content FROM docs WHERE docs MATCH '%s*' LIMIT %d;",
        esc, max_results);
    sqlite3_free(esc);

    sqlite3_stmt *stmt;
    int rc = sqlite3_prepare_v2(fts5_db, sql, -1, &stmt, NULL);
    sqlite3_free(sql);
    if (rc) return 0;

    int n = 0;
    out[0] = 0;
    while (sqlite3_step(stmt) == SQLITE_ROW && n < max_results) {
        const char *txt = (const char*)sqlite3_column_text(stmt, 0);
        if (txt) {
            strncat(out, txt, outsz - strlen(out) - 1);
            n++;
        }
    }
    sqlite3_finalize(stmt);
    return n;
}

static void fts5_close(void) {
    if (fts5_db) { sqlite3_close(fts5_db); fts5_db = NULL; }
}

/* ── Main benchmark ── */
#define N_DOCS     5000
#define N_QUERIES   200
#define MAX_STR      64

int main(void) {
    /* Ensure we're in the project directory */
    chdir("/data/data/com.termux/files/home/projects/database");

    /* Generate test data */
    static char docs[N_DOCS][MAX_STR];
    static char queries[N_QUERIES][MAX_STR];
    static int  query_is_typo[N_QUERIES]; /* 1 = typo'd version, 0 = exact city */
    static int  query_k[N_QUERIES];        /* edit distance for this query */

    xs64_state = 42;
    printf("Generating %d documents...\n", N_DOCS);
    for (int i = 0; i < N_DOCS; i++) {
        gen_city(docs[i], MAX_STR);
    }

    printf("Generating %d queries...\n", N_QUERIES);
    xs64_state = 123; /* different seed for queries */
    for (int i = 0; i < N_QUERIES; i++) {
        if (xs64() % 3 == 0) {
            /* Exact query: pick a random document */
            int d = (int)(xs64() % N_DOCS);
            strncpy(queries[i], docs[d], MAX_STR-1);
            queries[i][MAX_STR-1] = 0;
            query_is_typo[i] = 0;
            query_k[i] = 0;
        } else {
            /* Typo query: mutate a random document */
            int d = (int)(xs64() % N_DOCS);
            int k = 1 + (int)(xs64() % 2); /* ed=1 or ed=2 */
            mutate(docs[d], strlen(docs[d]), queries[i], k);
            query_is_typo[i] = 1;
            query_k[i] = k;
        }
    }

    /* ── Benchmark: SQLite FTS5 ── */
    printf("\n=== SQLite FTS5 ===\n");
    {
        double t0, t1;
        if (fts5_open("_bench_fts5.db") < 0) goto skip_fts5;

        /* Insert */
        t0 = now_sec();
        sqlite3_exec(fts5_db, "BEGIN TRANSACTION;", NULL, NULL, NULL);
        for (int i = 0; i < N_DOCS; i++) {
            fts5_insert(docs[i]);
        }
        sqlite3_exec(fts5_db, "COMMIT;", NULL, NULL, NULL);
        t1 = now_sec();
        printf("  Insert: %d docs in %.3fs = %.0f docs/s\n",
               N_DOCS, t1-t0, N_DOCS/(t1-t0));

        /* Search (prefix — FTS5 doesn't natively do fuzzy/typo) */
        int total_found = 0;
        t0 = now_sec();
        for (int i = 0; i < N_QUERIES; i++) {
            char result[4096];
            int n = fts5_search(queries[i], 20, result, sizeof(result));
            total_found += n;
        }
        t1 = now_sec();
        printf("  Search: %d queries in %.3fs = %.0f q/s (found %d, prefix match)\n",
               N_QUERIES, t1-t0, N_QUERIES/(t1-t0), total_found);

        /* Search with LIKE for substring (slower, but more comparable) */
        total_found = 0;
        t0 = now_sec();
        for (int i = 0; i < N_QUERIES; i++) {
            char *esc = sqlite3_mprintf("%q", queries[i]);
            char *sql = sqlite3_mprintf(
                "SELECT content FROM docs WHERE content LIKE '%%%s%%' LIMIT 20;", esc);
            sqlite3_free(esc);
            sqlite3_stmt *stmt;
            sqlite3_prepare_v2(fts5_db, sql, -1, &stmt, NULL);
            sqlite3_free(sql);
            int n = 0;
            while (sqlite3_step(stmt) == SQLITE_ROW && n < 20) {
                const char *txt = (const char*)sqlite3_column_text(stmt, 0);
                if (txt) { n++; }
            }
            sqlite3_finalize(stmt);
            total_found += n;
        }
        t1 = now_sec();
        printf("  Substr: %d queries in %.3fs = %.0f q/s (found %d, LIKE %%substr%%)\n",
               N_QUERIES, t1-t0, N_QUERIES/(t1-t0), total_found);
        
        fts5_close();
        unlink("_bench_fts5.db");
    }
skip_fts5:;

    /* ── Benchmark: ffdb ── */
    printf("\n=== ffdb (fractal fuzzy DB) ===\n");
    {
        char cmd[8192];
        unlink("data.ffdb");

        /* Insert via pipe */
        {
            FILE *fp = fopen("_bench_insert.txt", "w");
            for (int i = 0; i < N_DOCS; i++) {
                fprintf(fp, "add %s\n", docs[i]);
            }
            fclose(fp);

            double t0 = now_sec();
            snprintf(cmd, sizeof(cmd),
                ""
                "./ffdb serve < _bench_insert.txt > /dev/null 2>&1");
            shell_cmd(cmd);
            double t1 = now_sec();
            printf("  Insert: %d docs in %.3fs = %.0f docs/s\n",
                   N_DOCS, t1-t0, N_DOCS/(t1-t0));
        }

        /* Search via pipe */
        {
            FILE *fp = fopen("_bench_search.txt", "w");
            for (int i = 0; i < N_QUERIES; i++) {
                fprintf(fp, "search %s 2\n", queries[i]);
            }
            fclose(fp);

            double t0 = now_sec();
            /* Capture output to count results */
            snprintf(cmd, sizeof(cmd),
                ""
                "./ffdb serve < _bench_search.txt 2>/dev/null | grep -c 'results'");
            FILE *p = popen(cmd, "r");
            int total = 0;
            if (p) { fscanf(p, "%d", &total); pclose(p); }
            double t1 = now_sec();
            printf("  Search: %d queries in %.3fs = %.0f q/s (found ~%d)\n",
                   N_QUERIES, t1-t0, N_QUERIES/(t1-t0), total);
        }
        unlink("_bench_insert.txt");
        unlink("_bench_search.txt");
    }

    /* ── Benchmark: fuzzydb ── */
    printf("\n=== fuzzydb (embedded fuzzy DB) ===\n");
    {
        char cmd[8192];
        unlink("data.fuzzydb");

        FILE *fp = fopen("_bench_insert.txt", "w");
        for (int i = 0; i < N_DOCS; i++)
            fprintf(fp, "add %s\n", docs[i]);
        fclose(fp);

        double t0 = now_sec();
        snprintf(cmd, sizeof(cmd),
            ""
            "./fuzzydb serve < _bench_insert.txt > /dev/null 2>&1");
        shell_cmd(cmd);
        double t1 = now_sec();
        printf("  Insert: %d docs in %.3fs = %.0f docs/s\n",
               N_DOCS, t1-t0, N_DOCS/(t1-t0));

        fp = fopen("_bench_search.txt", "w");
        for (int i = 0; i < N_QUERIES; i++)
            fprintf(fp, "search %s 2\n", queries[i]);
        fclose(fp);

        t0 = now_sec();
        snprintf(cmd, sizeof(cmd),
            ""
            "./fuzzydb serve < _bench_search.txt 2>/dev/null | grep -c 'results'");
        FILE *p = popen(cmd, "r");
        int total = 0;
        if (p) { fscanf(p, "%d", &total); pclose(p); }
        t1 = now_sec();
        printf("  Search: %d queries in %.3fs = %.0f q/s (found ~%d)\n",
               N_QUERIES, t1-t0, N_QUERIES/(t1-t0), total);

        unlink("_bench_insert.txt");
        unlink("_bench_search.txt");
    }

    /* ── Benchmark: fastdb ── */
    printf("\n=== fastdb (columnar fuzzy DB) ===\n");
    {
        char cmd[16384];
        unlink("data.fastdb");

        /* Create table + import CSV via pipe */
        FILE *fp = fopen("_bench_fastdb.csv", "w");
        for (int i = 0; i < N_DOCS; i++)
            fprintf(fp, "%s\n", docs[i]);
        fclose(fp);

        /* Write commands to temp file */
        FILE *cf = fopen("_bench_fastdb_cmds.txt", "w");
        fprintf(cf, "create cities name:TEXT\n");
        fprintf(cf, "import cities _bench_fastdb.csv\n");
        fclose(cf);

        double t0 = now_sec();
        snprintf(cmd, sizeof(cmd),
            "./fastdb serve < _bench_fastdb_cmds.txt > /dev/null 2>&1");
        shell_cmd(cmd);
        double t1 = now_sec();
        printf("  Insert: %d rows in %.3fs = %.0f rows/s\n",
               N_DOCS, t1-t0, N_DOCS/(t1-t0));

        /* Fuzzy search — same pipe approach */
        fp = fopen("_bench_search.txt", "w");
        for (int i = 0; i < N_QUERIES; i++)
            fprintf(fp, "select cities fuzzy name %s 2\n", queries[i]);
        fclose(fp);

        t0 = now_sec();
        snprintf(cmd, sizeof(cmd),
            ""
            "./fastdb serve < _bench_search.txt 2>/dev/null | grep -c 'ed='");
        FILE *p = popen(cmd, "r");
        int total = 0;
        if (p) { fscanf(p, "%d", &total); pclose(p); }
        t1 = now_sec();
        printf("  Search: %d queries in %.3fs = %.0f q/s (found ~%d)\n",
               N_QUERIES, t1-t0, N_QUERIES/(t1-t0), total);

        unlink("_bench_fastdb.csv");
        unlink("_bench_search.txt");
    }

    /* ── Summary ── */
    printf("\n=== Summary ===\n");
    printf("  Test data: %d docs (city names), %d queries (exact + typo ed≤2)\n",
           N_DOCS, N_QUERIES);
    printf("  SQLite FTS5:   prefix match only (no edit distance)\n");
    printf("  ffdb/fuzzydb:  trigram + Myers/DL edit distance\n");
    printf("  fastdb:        columnar, trigram + Myers/DL edit distance\n");
    printf("\n  Benchmarks include process startup overhead (shell out).\n");
    printf("  For pure in-process speed, see bench_ed.c\n");

    return 0;
}
