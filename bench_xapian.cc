/*
 * bench_xapian.cc — Xapian 2.0 vs ffdb benchmark
 *
 * Compile:
 *   g++ -O3 -march=armv8-a -Wall -Wextra -Werror \
 *       -o bench_xapian bench_xapian.cc \
 *       $(pkg-config --cflags --libs xapian-core) -I.
 */
#include <xapian.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <ctype.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>

/* ── PRNG ── */
static uint64_t xs64_state;
static uint64_t xs64(void) {
    uint64_t x = xs64_state;
    x ^= x << 13; x ^= x >> 7; x ^= x << 17;
    return xs64_state = x;
}

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

static int shell_cmd(const char *cmd) { return system(cmd); }

#define N_DOCS    5000
#define N_QUERIES  200
#define MAX_STR     64

int main() {
    chdir("/data/data/com.termux/files/home/projects/database");

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

    printf("=== Xapian 2.0 vs ffdb: %d docs, %d queries ===\n\n", N_DOCS, N_QUERIES);

    double t0, t1;
    int total;
    char cmd[16384];

    /* ── XAPIAN ── */
    printf("── Xapian 2.0 (C++, BM25, stemming, wildcards) ──\n");
    {
        unlink("_bench_xapian");
        Xapian::WritableDatabase db = Xapian::WritableDatabase("_bench_xapian",
            Xapian::DB_CREATE_OR_OVERWRITE);

        /* Insert with term generation + stemming */
        t0 = now_sec();
        Xapian::TermGenerator tg;
        tg.set_stemmer(Xapian::Stem("en"));
        for (int i = 0; i < N_DOCS; i++) {
            Xapian::Document doc;
            tg.set_document(doc);
            tg.index_text(docs[i]);  /* tokenize + stem + index */
            doc.set_data(docs[i]);  /* store original */
            db.add_document(doc);
        }
        db.commit();
        t1 = now_sec();
        printf("  Insert: %d docs in %.3fs = %.0f docs/s (stemmed)\n",
               N_DOCS, t1-t0, N_DOCS/(t1-t0));

        /* Term search (exact token match) */
        Xapian::QueryParser qp;
        qp.set_stemmer(Xapian::Stem("en"));
        qp.set_database(db);

        total = 0;
        t0 = now_sec();
        for (int i = 0; i < N_QUERIES; i++) {
            /* Lowercase + use as term (no wildcards) */
            char low[MAX_STR];
            for (int j = 0; queries[i][j]; j++)
                low[j] = (char)tolower((uint8_t)queries[i][j]);
            low[strlen(queries[i])] = 0;

            Xapian::Query q = qp.parse_query(low,
                Xapian::QueryParser::FLAG_DEFAULT |
                Xapian::QueryParser::FLAG_PHRASE);
            Xapian::Enquire enquire(db);
            enquire.set_query(q);
            Xapian::MSet mset = enquire.get_mset(0, 20);
            total += mset.size();
        }
        t1 = now_sec();
        printf("  Term:  %d queries in %.3fs = %.0f q/s (found %d, stemmed exact)\n",
               N_QUERIES, t1-t0, N_QUERIES/(t1-t0), total);

        /* Wildcard search (prefix + edit distance via spelling) */
        total = 0;
        t0 = now_sec();
        for (int i = 0; i < N_QUERIES; i++) {
            char low[MAX_STR];
            for (int j = 0; queries[i][j]; j++)
                low[j] = (char)tolower((uint8_t)queries[i][j]);
            low[strlen(queries[i])] = 0;

            /* Use wildcard + FLAG_SPELLING_CORRECTION for typo tolerance */
            Xapian::Query q = qp.parse_query(low,
                Xapian::QueryParser::FLAG_WILDCARD |
                Xapian::QueryParser::FLAG_SPELLING_CORRECTION);
            Xapian::Enquire enquire(db);
            enquire.set_query(q);
            Xapian::MSet mset = enquire.get_mset(0, 20);
            total += mset.size();
        }
        t1 = now_sec();
        printf("  Fuzzy: %d queries in %.3fs = %.0f q/s (found %d, wildcard+spell)\n",
               N_QUERIES, t1-t0, N_QUERIES/(t1-t0), total);

        db.close();
    }

    /* ── FFDB ── */
    printf("\n── ffdb (fractal fuzzy: trigram + Myers/DL) ──\n");
    {
        unlink("data.ffdb");
        FILE *fp = fopen("_bench_ins.txt", "w");
        for (int i = 0; i < N_DOCS; i++) fprintf(fp, "add %s\n", docs[i]);
        fclose(fp);

        t0 = now_sec();
        shell_cmd("./ffdb serve < _bench_ins.txt > /dev/null 2>&1");
        t1 = now_sec();
        printf("  Insert: %d docs in %.3fs = %.0f docs/s\n",
               N_DOCS, t1-t0, N_DOCS/(t1-t0));

        /* Fuzzy search */
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

        /* Substring search */
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

    /* ── Summary ── */
    printf("\n╔════════════════════════════════════════════╗\n");
    printf("║  Xapian:  C++, BM25, stemming, wildcards ║\n");
    printf("║  ffdb:    C, trigram + Myers/DL fuzzy    ║\n");
    printf("╚════════════════════════════════════════════╝\n");

    return 0;
}
