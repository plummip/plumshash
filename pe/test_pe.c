/*
 * test_pe.c — smoke test for the Project Editor library
 *
 * Tests: caching, insert, delete, replace, dependencies, flush, sync.
 * Creates a temp project under /data/data/com.termux/files/home/tmp/pe_test/
 * (Termux /tmp is not writable).
 */

#include "pe.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#define TESTDIR "/data/data/com.termux/files/home/tmp/pe_test"

static void setup_project(void) {
    system("rm -rf " TESTDIR);
    mkdir(TESTDIR, 0755);

    FILE *f;

    f = fopen(TESTDIR "/main.c", "w");
    fprintf(f, "// main.c\nint main() {\n    return 0;\n}\n");
    fclose(f);

    f = fopen(TESTDIR "/util.c", "w");
    fprintf(f, "// util.c\nint add(int a, int b) {\n    return a + b;\n}\n");
    fclose(f);

    f = fopen(TESTDIR "/empty.c", "w");
    fclose(f);
}

static void assert_file_eq(const char *rel, const char *expect, size_t elen) {
    char full[1024];
    snprintf(full, sizeof(full), TESTDIR "/%s", rel);
    FILE *f = fopen(full, "rb");
    assert(f);
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    rewind(f);
    char *buf = malloc(sz + 1);
    size_t n = fread(buf, 1, sz, f);
    fclose(f);
    buf[n] = '\0';

    if (n != elen || memcmp(buf, expect, elen) != 0) {
        fprintf(stderr, "FAIL: %s\n", rel);
        fprintf(stderr, "  expected (%zu bytes):\n---\n%s\n---\n", elen, expect);
        fprintf(stderr, "  got (%zu bytes):\n---\n%s\n---\n", n, buf);
        free(buf);
        exit(1);
    }
    free(buf);
}

int main(void) {
    setup_project();

    /* ── Create editor ── */
    pe_t *pe = pe_create(TESTDIR, 2);
    assert(pe);

    /* ── Cache files ── */
    assert(pe_cache_file(pe, "main.c") == 0);
    assert(pe_cache_file(pe, "util.c") == 0);
    assert(pe_cache_file(pe, "empty.c") == 0);

    /* Verify line counts */
    assert(pe_file_lines(pe, "main.c") == 4);
    assert(pe_file_lines(pe, "util.c") == 4);
    assert(pe_file_lines(pe, "empty.c") == 1);

    /* ── Edit 1: INSERT #include before line 1 of main.c ── */
    pe_edit_t *e1 = pe_edit_create(pe, "main.c", PE_OP_INSERT,
        (pe_pos_t){1, 1}, (pe_pos_t){0, 0},
        "#include \"util.h\"\n", 18);
    assert(e1);
    assert(pe_edit_submit(pe, e1) == 0);

    /* ── Edit 2: DELETE first line of util.c (independent) ── */
    pe_edit_t *e2 = pe_edit_create(pe, "util.c", PE_OP_DELETE,
        (pe_pos_t){1, 1}, (pe_pos_t){2, 1}, NULL, 0);
    assert(e2);
    assert(pe_edit_submit(pe, e2) == 0);

    /* ── Edit 3: REPLACE return line in main.c (depends on e1) ── */
    pe_edit_t *e3 = pe_edit_create(pe, "main.c", PE_OP_REPLACE,
        (pe_pos_t){4, 1}, (pe_pos_t){5, 1},
        "    return add(1, 2);\n", 22);
    assert(e3);
    assert(pe_edit_depend(e3, e1) == 0);
    assert(pe_edit_submit(pe, e3) == 0);

    /* ── Edit 4: INSERT into empty file ── */
    pe_edit_t *e4 = pe_edit_create(pe, "empty.c", PE_OP_INSERT,
        (pe_pos_t){1, 1}, (pe_pos_t){0, 0},
        "/* was empty */\n", 16);
    assert(e4);
    assert(pe_edit_submit(pe, e4) == 0);

    /* ── Flush ── */
    size_t failed = pe_flush(pe);
    if (failed != 0) {
        fprintf(stderr, "FAIL: %zu edits failed\n", failed);
        if (pe_edit_state(e1) == PE_EDIT_FAILED)
            fprintf(stderr, "  e1: %s\n", pe_edit_error(e1));
        if (pe_edit_state(e2) == PE_EDIT_FAILED)
            fprintf(stderr, "  e2: %s\n", pe_edit_error(e2));
        if (pe_edit_state(e3) == PE_EDIT_FAILED)
            fprintf(stderr, "  e3: %s\n", pe_edit_error(e3));
        exit(1);
    }

    assert(pe_edit_state(e1) == PE_EDIT_DONE);
    assert(pe_edit_state(e2) == PE_EDIT_DONE);
    assert(pe_edit_state(e3) == PE_EDIT_DONE);
    assert(pe_edit_state(e4) == PE_EDIT_DONE);

    /* ── Verify in-memory cache ── */
    size_t sz;
    const char *data;

    data = pe_file_data(pe, "main.c", &sz);
    assert(data);
    const char *expect_main =
        "#include \"util.h\"\n"
        "// main.c\n"
        "int main() {\n"
        "    return add(1, 2);\n"
        "}\n";
    assert(sz == strlen(expect_main));
    assert(memcmp(data, expect_main, sz) == 0);

    data = pe_file_data(pe, "util.c", &sz);
    assert(data);
    const char *expect_util =
        "int add(int a, int b) {\n"
        "    return a + b;\n"
        "}\n";
    assert(sz == strlen(expect_util));
    assert(memcmp(data, expect_util, sz) == 0);

    data = pe_file_data(pe, "empty.c", &sz);
    assert(data);
    assert(sz == 16);
    assert(memcmp(data, "/* was empty */\n", 16) == 0);

    /* ── Sync to disk ── */
    assert(pe_sync(pe) == 0);

    /* ── Verify on-disk ── */
    assert_file_eq("main.c", expect_main, strlen(expect_main));
    assert_file_eq("util.c", expect_util, strlen(expect_util));

    /* ── Transaction test ── */
    assert(pe_txn_begin(pe) == 0);
    pe_edit_t *t1 = pe_edit_create(pe, "main.c", PE_OP_INSERT,
        (pe_pos_t){1, 1}, (pe_pos_t){0, 0}, "/* txn */\n", 10);
    assert(t1);
    assert(pe_edit_submit(pe, t1) == 0);
    assert(pe_txn_commit(pe) == 0);

    /* Rollback */
    assert(pe_txn_begin(pe) == 0);
    pe_edit_t *t2 = pe_edit_create(pe, "main.c", PE_OP_INSERT,
        (pe_pos_t){1, 1}, (pe_pos_t){0, 0}, "// GONE\n", 8);
    assert(t2);
    assert(pe_edit_submit(pe, t2) == 0);
    pe_txn_rollback(pe);
    assert(!pe_txn_active(pe));
    data = pe_file_data(pe, "main.c", &sz);
    assert(strstr(data, "GONE") == NULL);

    /* ── Undo test ── */
    assert(pe_undo_depth(pe, "main.c") == 3);  /* e1 + e3 + t1 txn insert */
    assert(pe_undo(pe, "main.c") == 0);         /* undo t1 (txn insert) */
    assert(pe_undo(pe, "main.c") == 0);         /* undo e3 (replace) */
    assert(pe_undo(pe, "main.c") == 0);         /* undo e1 (insert) */
    assert(pe_undo_depth(pe, "main.c") == 0);
    data = pe_file_data(pe, "main.c", &sz);
    assert(data && strstr(data, "#include") == NULL);
    assert(strstr(data, "return 0;") != NULL);

    /* ── Diff test ── */
    char *diff = pe_diff(pe, "main.c", 3);
    assert(diff);
    assert(strstr(diff, "--- a/main.c") != NULL);
    free(diff);

    /* ── Cleanup ── */
    pe_destroy(pe);
    system("rm -rf " TESTDIR);

    printf("pe: all tests passed\n");
    return 0;
}
