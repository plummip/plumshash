/*
 * bench_ft.c — Full-text database showdown:
 *   SQLite FTS5 vs ffdb vs fuzzydb vs minimal inverted index vs grep
 *
 * Compile:
 *   gcc -O3 -march=armv8-a -Wall -Wextra -Werror \
 *       -o bench_ft bench_ft.c -lsqlite3 -I.
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

/* City name pool (85 cities) */
static const char *cities[] = {
    "Amsterdam","Rotterdam","Utrecht","Eindhoven","Groningen",
    "London","Manchester","Birmingham","Liverpool","Bristol",
    "Paris","Marseille","Lyon","Toulouse","Bordeaux",
    "Berlin","Hamburg","Munich","Cologne","Frankfurt",
    "Madrid","Barcelona","Valencia","Seville","Bilbao",
    "Rome","Milan","Naples","Turin","Florence",
    "NewYork","LosAngeles","Chicago","Houston","Phoenix",
    "Tokyo","Osaka","Kyoto","Nagoya","Sapporo",
    "Sydney","Melbourne","Brisbane","Perth","Adelaide",
    "Moscow","SaintPetersburg","Novosibirsk","Yekaterinburg","Kazan",
    "Beijing","Shanghai","Shenzhen","Guangzhou","Chengdu",
    "Mumbai","Delhi","Bangalore","Hyderabad","Chennai",
    "Cairo","Lagos","Nairobi","CapeTown","Johannesburg",
    "Toronto","Vancouver","Montreal","Ottawa","Calgary",
    "MexicoCity","BuenosAires","SaoPaulo","Lima","Santiago",
    "Stockholm","Oslo","Copenhagen","Helsinki","Reykjavik",
    "Warsaw","Prague","Vienna","Budapest","Bucharest",
    "Istanbul","Ankara","Tehran","Baghdad","Riyadh",
    "Seoul","Bangkok","Jakarta","Manila","Hanoi"
};
#define N_CITIES 85

static void gen_city(char *buf, int maxlen) {
    if (xs64() % 4 == 0) {
        int len = 5 + (int)(xs64() % 11);
        for (int i = 0; i < len && i < maxlen-1; i++)
            buf[i] = 'a' + (int)(xs64() % 26);
        buf[len < maxlen-1 ? len : maxlen-1] = 0;
    } else {
        const char *c = cities[xs64() % N_CITIES];
        int cl = strlen(c);
        if (cl >= maxlen) cl = maxlen - 1;
        memcpy(buf, c, cl); buf[cl] = 0;
    }
}

static int mutate(const char *src, int slen, char *dst, int k) {
    memcpy(dst, src, slen);
    int dlen = slen;
    for (int e = 0; e < k; e++) {
        if (dlen <= 1) { dst[0] = 'a'+(int)(xs64()%26); break; }
        if (dlen >= 58) break;
        int op = (int)(xs64()%3), pos = (int)(xs64()%dlen);
        switch(op){
        case 0: dst[pos]='a'+(int)(xs64()%26); break;
        case 1: if(pos<dlen-1){memmove(dst+pos,dst+pos+1,dlen-pos-1);dlen--;}break;
        case 2: memmove(dst+pos+1,dst+pos,dlen-pos);dst[pos]='a'+(int)(xs64()%26);dlen++;break;
        }
    }
    dst[dlen]=0; return dlen;
}

static double now_sec(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

/* ═══════════════════════════════════════════════════════════════
 * MINIMAL INVERTED INDEX (speed-of-light baseline)
 * Tokenization: split on whitespace, lowercase. Exact match only.
 * No edit distance, no fuzzy, no scoring — just raw inverted index.
 * ═══════════════════════════════════════════════════════════════ */

#define MI_MAX_DOCS   100000
#define MI_MAX_TOKENS 500000
#define MI_MAX_WORD     64

typedef struct {
    uint32_t token_hash;  /* FNV-1a hash of lowercase word */
    uint32_t doc_id;
    uint32_t next;        /* index into postings[] for next occurrence */
} MIPosting;

typedef struct {
    const char *text;     /* points into docs[] */
    uint16_t len;
} MIDoc;

static MIDoc    *mi_docs      = NULL;
static uint32_t  mi_ndocs     = 0;
static MIPosting *mi_postings = NULL;
static uint32_t  mi_npostings = 0;
static uint32_t *mi_dict      = NULL;  /* hash→posting_index, 65536 slots */
static uint32_t  mi_dict_mask = 65535;

static uint32_t mi_fnv1a(const char *s, int len) {
    uint32_t h = 2166136261U;
    for (int i = 0; i < len; i++) h = (h ^ (uint8_t)s[i]) * 16777619U;
    return h;
}

static void mi_init(void) {
    mi_docs    = calloc(MI_MAX_DOCS, sizeof(MIDoc));
    mi_postings= calloc(MI_MAX_TOKENS, sizeof(MIPosting));
    mi_dict    = calloc(65536, sizeof(uint32_t));
    mi_ndocs = 0; mi_npostings = 0;
    memset(mi_dict, 0, 65536 * sizeof(uint32_t));
}

static void mi_add(const char *text) {
    if (mi_ndocs >= MI_MAX_DOCS) return;
    int tl = strlen(text);
    mi_docs[mi_ndocs].text = strdup(text);
    mi_docs[mi_ndocs].len  = (uint16_t)(tl < 65535 ? tl : 65535);

    /* Tokenize: split on non-alnum */
    char word[MI_MAX_WORD];
    int wl = 0;
    for (int i = 0; i <= tl; i++) {
        char c = (i < tl) ? (char)tolower((uint8_t)text[i]) : 0;
        if (isalnum((uint8_t)c) && wl < MI_MAX_WORD-1) {
            word[wl++] = c;
        } else if (wl > 0) {
            word[wl] = 0;
            uint32_t h = mi_fnv1a(word, wl);
            uint32_t slot = h & mi_dict_mask;

            if (mi_npostings < MI_MAX_TOKENS) {
                mi_postings[mi_npostings].token_hash = h;
                mi_postings[mi_npostings].doc_id = mi_ndocs;
                mi_postings[mi_npostings].next = mi_dict[slot];
                mi_dict[slot] = mi_npostings;
                mi_npostings++;
            }
            wl = 0;
        }
    }
    mi_ndocs++;
}

/* Exact token search: find docs containing ALL query tokens */
static int mi_search(const char *query, uint32_t *results, int max_results) {
    /* Tokenize query */
    char *words[32]; int nw = 0;
    char qcopy[256]; strncpy(qcopy, query, 255); qcopy[255]=0;
    char *tok = strtok(qcopy, " ");
    while (tok && nw < 32) {
        /* lowercase */
        for (char *p = tok; *p; p++) *p = (char)tolower((uint8_t)*p);
        words[nw++] = tok;
        tok = strtok(NULL, " ");
    }
    if (nw == 0) return 0;

    /* Get posting list for first word */
    uint32_t h0 = mi_fnv1a(words[0], strlen(words[0]));
    uint32_t slot = h0 & mi_dict_mask;
    uint32_t pi = mi_dict[slot];

    /* Collect doc IDs from first word */
    uint32_t candidates[1024]; int nc = 0;
    while (pi && nc < 1024) {
        if (mi_postings[pi-1].token_hash == h0) {
            candidates[nc++] = mi_postings[pi-1].doc_id;
        }
        pi = mi_postings[pi-1].next;
    }

    /* Intersect with remaining words */
    for (int w = 1; w < nw && nc > 0; w++) {
        uint32_t hw = mi_fnv1a(words[w], strlen(words[w]));
        uint32_t slot_w = hw & mi_dict_mask;
        /* Build bitset of docs containing this word */
        uint8_t has_word[16384] = {0}; /* covers up to 131072 docs */
        pi = mi_dict[slot_w];
        while (pi) {
            if (mi_postings[pi-1].token_hash == hw) {
                uint32_t d = mi_postings[pi-1].doc_id;
                if (d < 131072) has_word[d >> 3] |= (uint8_t)(1 << (d & 7));
            }
            pi = mi_postings[pi-1].next;
        }
        /* Filter candidates */
        int out = 0;
        for (int i = 0; i < nc; i++) {
            uint32_t d = candidates[i];
            if (d < 131072 && (has_word[d >> 3] & (1 << (d & 7)))) {
                candidates[out++] = d;
            }
        }
        nc = out;
    }

    int out = 0;
    for (int i = 0; i < nc && out < max_results; i++)
        results[out++] = candidates[i];
    return out;
}

static void mi_free(void) {
    for (uint32_t i = 0; i < mi_ndocs; i++) free((void*)mi_docs[i].text);
    free(mi_docs); free(mi_postings); free(mi_dict);
    mi_docs = NULL; mi_postings = NULL; mi_dict = NULL;
}

/* ═══════════════════════════════════════════════════════════════
 * BENCHMARK
 * ═══════════════════════════════════════════════════════════════ */

#define N_DOCS    5000
#define N_QUERIES  200
#define MAX_STR     64

static int shell_cmd(const char *cmd) { return system(cmd); }

int main(void) {
    chdir("/data/data/com.termux/files/home/projects/database");

    /* Generate data */
    static char docs[N_DOCS][MAX_STR];
    static char queries[N_QUERIES][MAX_STR];
    static int  query_k[N_QUERIES];

    xs64_state = 42;
    for (int i = 0; i < N_DOCS; i++) gen_city(docs[i], MAX_STR);

    xs64_state = 123;
    for (int i = 0; i < N_QUERIES; i++) {
        int d = (int)(xs64() % N_DOCS);
        int k = (xs64() % 3 == 0) ? 0 : 1 + (int)(xs64() % 2);
        if (k == 0) {
            strncpy(queries[i], docs[d], MAX_STR-1);
            queries[i][MAX_STR-1] = 0;
        } else {
            mutate(docs[d], strlen(docs[d]), queries[i], k);
        }
        query_k[i] = k;
    }

    printf("=== Benchmark: %d docs, %d queries ===\n\n", N_DOCS, N_QUERIES);

    double t0, t1;
    int total;
    char cmd[16384];

    /* ── 1. Minimal inverted index ── */
    printf("── Minimal inverted index (exact token match only) ──\n");
    mi_init();
    t0 = now_sec();
    for (int i = 0; i < N_DOCS; i++) mi_add(docs[i]);
    t1 = now_sec();
    printf("  Insert: %d docs in %.3fs = %.0f docs/s\n", N_DOCS, t1-t0, N_DOCS/(t1-t0));

    total = 0;
    t0 = now_sec();
    for (int i = 0; i < N_QUERIES; i++) {
        uint32_t results[20];
        total += mi_search(queries[i], results, 20);
    }
    t1 = now_sec();
    printf("  Search: %d queries in %.3fs = %.0f q/s (found %d)\n",
           N_QUERIES, t1-t0, N_QUERIES/(t1-t0), total);
    mi_free();

    /* ── 2. grep -c (substring baseline) ── */
    printf("\n── grep -c (substring count, no index) ──\n");
    {
        FILE *fp = fopen("_bench_grep.txt", "w");
        for (int i = 0; i < N_DOCS; i++) fprintf(fp, "%s\n", docs[i]);
        fclose(fp);

        total = 0;
        t0 = now_sec();
        for (int i = 0; i < N_QUERIES; i++) {
            snprintf(cmd, sizeof(cmd),
                "grep -c '%s' _bench_grep.txt 2>/dev/null", queries[i]);
            FILE *p = popen(cmd, "r");
            int c = 0;
            if (p) { fscanf(p, "%d", &c); pclose(p); }
            total += c;
        }
        t1 = now_sec();
        printf("  Search: %d queries in %.3fs = %.0f q/s (found %d, substring)\n",
               N_QUERIES, t1-t0, N_QUERIES/(t1-t0), total);
        unlink("_bench_grep.txt");
    }

    /* ── 3. ripgrep (if available) ── */
    printf("\n── rg (ripgrep, if available) ──\n");
    {
        FILE *fp = fopen("_bench_grep.txt", "w");
        for (int i = 0; i < N_DOCS; i++) fprintf(fp, "%s\n", docs[i]);
        fclose(fp);

        int has_rg = (system("which rg >/dev/null 2>&1") == 0);
        if (has_rg) {
            total = 0;
            t0 = now_sec();
            for (int i = 0; i < N_QUERIES; i++) {
                snprintf(cmd, sizeof(cmd),
                    "rg -c '%s' _bench_grep.txt 2>/dev/null", queries[i]);
                FILE *p = popen(cmd, "r");
                int c = 0;
                if (p) { fscanf(p, "%d", &c); pclose(p); }
                total += c;
            }
            t1 = now_sec();
            printf("  Search: %d queries in %.3fs = %.0f q/s (found %d, substring)\n",
                   N_QUERIES, t1-t0, N_QUERIES/(t1-t0), total);
        } else {
            printf("  (not installed — skip)\n");
        }
        unlink("_bench_grep.txt");
    }

    /* ── 4. SQLite FTS5 ── */
    printf("\n── SQLite FTS5 ──\n");
    {
        sqlite3 *db;
        unlink("_bench_fts5.db");
        sqlite3_open("_bench_fts5.db", &db);
        sqlite3_exec(db, "CREATE VIRTUAL TABLE docs USING fts5(content);",0,0,0);

        t0 = now_sec();
        sqlite3_exec(db, "BEGIN;",0,0,0);
        for (int i = 0; i < N_DOCS; i++) {
            char *esc = sqlite3_mprintf("%q", docs[i]);
            char *sql = sqlite3_mprintf("INSERT INTO docs VALUES('%s');", esc);
            sqlite3_exec(db, sql, 0,0,0);
            sqlite3_free(sql); sqlite3_free(esc);
        }
        sqlite3_exec(db, "COMMIT;",0,0,0);
        t1 = now_sec();
        printf("  Insert: %d docs in %.3fs = %.0f docs/s\n", N_DOCS, t1-t0, N_DOCS/(t1-t0));

        /* Prefix search */
        total = 0;
        t0 = now_sec();
        for (int i = 0; i < N_QUERIES; i++) {
            char *esc = sqlite3_mprintf("%q", queries[i]);
            char *sql = sqlite3_mprintf(
                "SELECT count(*) FROM docs WHERE docs MATCH '%s*';", esc);
            sqlite3_free(esc);
            sqlite3_stmt *stmt;
            sqlite3_prepare_v2(db, sql, -1, &stmt, 0);
            sqlite3_free(sql);
            if (sqlite3_step(stmt) == SQLITE_ROW)
                total += sqlite3_column_int(stmt, 0);
            sqlite3_finalize(stmt);
        }
        t1 = now_sec();
        printf("  Prefix: %d queries in %.3fs = %.0f q/s (found %d)\n",
               N_QUERIES, t1-t0, N_QUERIES/(t1-t0), total);

        /* LIKE substring */
        total = 0;
        t0 = now_sec();
        for (int i = 0; i < N_QUERIES; i++) {
            char *esc = sqlite3_mprintf("%q", queries[i]);
            char *sql = sqlite3_mprintf(
                "SELECT count(*) FROM docs WHERE content LIKE '%%%s%%';", esc);
            sqlite3_free(esc);
            sqlite3_stmt *stmt;
            sqlite3_prepare_v2(db, sql, -1, &stmt, 0);
            sqlite3_free(sql);
            if (sqlite3_step(stmt) == SQLITE_ROW)
                total += sqlite3_column_int(stmt, 0);
            sqlite3_finalize(stmt);
        }
        t1 = now_sec();
        printf("  LIKE:  %d queries in %.3fs = %.0f q/s (found %d)\n",
               N_QUERIES, t1-t0, N_QUERIES/(t1-t0), total);

        sqlite3_close(db);
        unlink("_bench_fts5.db");
    }

    /* ── 5. ffdb (fuzzy) ── */
    printf("\n── ffdb (fractal fuzzy: trigram + Myers/DL) ──\n");
    {
        unlink("data.ffdb");
        FILE *fp = fopen("_bench_ins.txt", "w");
        for (int i = 0; i < N_DOCS; i++) fprintf(fp, "add %s\n", docs[i]);
        fclose(fp);

        t0 = now_sec();
        shell_cmd("./ffdb serve < _bench_ins.txt > /dev/null 2>&1");
        t1 = now_sec();
        printf("  Insert: %d docs in %.3fs = %.0f docs/s\n", N_DOCS, t1-t0, N_DOCS/(t1-t0));

        fp = fopen("_bench_src.txt", "w");
        for (int i = 0; i < N_QUERIES; i++)
            fprintf(fp, "search %s 2\n", queries[i]);
        fclose(fp);

        t0 = now_sec();
        snprintf(cmd, sizeof(cmd),
            "./ffdb serve < _bench_src.txt 2>/dev/null | grep -c 'results'");
        FILE *p = popen(cmd, "r");
        total = 0;
        if (p) { fscanf(p, "%d", &total); pclose(p); }
        t1 = now_sec();
        printf("  Fuzzy: %d queries in %.3fs = %.0f q/s (found ~%d, ed≤2)\n",
               N_QUERIES, t1-t0, N_QUERIES/(t1-t0), total);

        /* Also test substring search */
        fp = fopen("_bench_src.txt", "w");
        for (int i = 0; i < N_QUERIES; i++)
            fprintf(fp, "find %s\n", queries[i]);
        fclose(fp);

        t0 = now_sec();
        snprintf(cmd, sizeof(cmd),
            "./ffdb serve < _bench_src.txt 2>/dev/null | grep -c 'results'");
        p = popen(cmd, "r");
        total = 0;
        if (p) { fscanf(p, "%d", &total); pclose(p); }
        t1 = now_sec();
        printf("  Substr:%d queries in %.3fs = %.0f q/s (found ~%d)\n",
               N_QUERIES, t1-t0, N_QUERIES/(t1-t0), total);

        unlink("_bench_ins.txt"); unlink("_bench_src.txt");
    }

    /* ── 6. fuzzydb (fuzzy) ── */
    printf("\n── fuzzydb (trigram + Myers/DL) ──\n");
    {
        unlink("data.fuzzydb");
        FILE *fp = fopen("_bench_ins.txt", "w");
        for (int i = 0; i < N_DOCS; i++) fprintf(fp, "add %s\n", docs[i]);
        fclose(fp);

        t0 = now_sec();
        shell_cmd("./fuzzydb serve < _bench_ins.txt > /dev/null 2>&1");
        t1 = now_sec();
        printf("  Insert: %d docs in %.3fs = %.0f docs/s\n", N_DOCS, t1-t0, N_DOCS/(t1-t0));

        fp = fopen("_bench_src.txt", "w");
        for (int i = 0; i < N_QUERIES; i++)
            fprintf(fp, "search %s 2\n", queries[i]);
        fclose(fp);

        t0 = now_sec();
        snprintf(cmd, sizeof(cmd),
            "./fuzzydb serve < _bench_src.txt 2>/dev/null | grep -c 'results'");
        FILE *p = popen(cmd, "r");
        total = 0;
        if (p) { fscanf(p, "%d", &total); pclose(p); }
        t1 = now_sec();
        printf("  Fuzzy: %d queries in %.3fs = %.0f q/s (found ~%d, ed≤2)\n",
               N_QUERIES, t1-t0, N_QUERIES/(t1-t0), total);

        unlink("_bench_ins.txt"); unlink("_bench_src.txt");
    }

    /* ── Summary ── */
    printf("\n╔══════════════════════════════════════════════════════════════╗\n");
    printf("║  SUMMARY: %d docs, %d queries (exact + typo ed≤2)     ║\n", N_DOCS, N_QUERIES);
    printf("╠══════════════════════════════════════════════════════════════╣\n");
    printf("║  All times include process startup overhead (shell out).   ║\n");
    printf("║  'Fuzzy' = edit distance with trigram pre-filter.          ║\n");
    printf("║  'Prefix' = token prefix match (no typo tolerance).        ║\n");
    printf("║  'Substr' = literal substring (no typo tolerance).         ║\n");
    printf("║  Minimal inverted index is in-process C (no shell out).    ║\n");
    printf("╚══════════════════════════════════════════════════════════════╝\n");

    return 0;
}
