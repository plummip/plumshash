/*
 * test_concur.c — concurrency stress test for Project Editor
 *
 * Spawns N threads, each making multiple edits to a shared file.
 * Verifies that pe_flush returns 0 (no failures) and final
 * file content matches expected.
 */
#include "pe.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <pthread.h>
#include <sys/stat.h>
#include <unistd.h>

#define TD        "/data/data/com.termux/files/home/tmp/pe_concur"
#define N_THREADS  6
#define EDITS_PER  200
/* "T00:00000\n" = exactly 10 bytes */
#define FMT_LEN    10

static void setup(void) {
    system("rm -rf " TD);
    mkdir(TD, 0755);
    /* Empty file to start */
    FILE *f = fopen(TD "/f.txt", "w");
    fclose(f);
}

typedef struct {
    pe_t *pe;
    int   id;
} worker_arg_t;

static void *worker(void *arg) {
    worker_arg_t *a = (worker_arg_t *)arg;
    char buf[FMT_LEN + 1];

    for (int i = 0; i < EDITS_PER; i++) {
        int n = snprintf(buf, sizeof(buf), "T%02d:%05d\n", a->id, i);
        assert(n == FMT_LEN);
        pe_edit_t *e = pe_edit_create(a->pe, "f.txt", PE_OP_INSERT,
            (pe_pos_t){1, 1}, (pe_pos_t){0, 0}, buf, (size_t)n);
        if (!e) { fprintf(stderr, "thread %d: create failed\n", a->id); return (void *)-1; }
        if (pe_edit_submit(a->pe, e) != 0) {
            fprintf(stderr, "thread %d: submit failed at %d\n", a->id, i);
            return (void *)-1;
        }
    }
    return NULL;
}

int main(void) {
    setup();

    /* 1 worker = 1 internal pool thread handling all edits.
     * This forces serialization through the ready queue. */
    pe_t *pe = pe_create(TD, 1);
    assert(pe);
    assert(pe_cache_file(pe, "f.txt") == 0);

    pthread_t threads[N_THREADS];
    worker_arg_t args[N_THREADS];

    for (int i = 0; i < N_THREADS; i++) {
        args[i].pe = pe;
        args[i].id = i;
        assert(pthread_create(&threads[i], NULL, worker, &args[i]) == 0);
    }

    for (int i = 0; i < N_THREADS; i++) {
        void *ret;
        pthread_join(threads[i], &ret);
        assert(ret == NULL);
    }

    /* Flush — must have zero failures */
    size_t nf = pe_flush(pe);
    if (nf != 0) {
        fprintf(stderr, "FAIL: %zu edits failed\n", nf);
        exit(1);
    }

    /* Verify file has all lines */
    size_t sz;
    const char *data = pe_file_data(pe, "f.txt", &sz);
    assert(data);

    size_t expected = (size_t)N_THREADS * EDITS_PER * FMT_LEN;
    assert(sz == expected);

    /* Verify each thread's lines are present (spot check) */
    assert(strstr(data, "T00:00000") != NULL);
    assert(strstr(data, "T05:00199") != NULL);

    /* Persist to disk before verifying */
    assert(pe_sync(pe) == 0);
    pe_destroy(pe);
    /* Re-open and verify on disk */
    pe = pe_create(TD, 1);
    assert(pe);
    assert(pe_cache_file(pe, "f.txt") == 0);
    data = pe_file_data(pe, "f.txt", &sz);
    assert(data && sz == expected);
    pe_destroy(pe);

    system("rm -rf " TD);
    printf("test_concur: all tests passed (%zu inserts)\n", expected / FMT_LEN);
    return 0;
}
