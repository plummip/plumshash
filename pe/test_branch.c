/*
 * test_branch.c — branch tests for Project Editor
 * Verifies: create, switch, merge (into target!), delete, conflicts
 */
#include "pe.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <sys/stat.h>
#include <unistd.h>

#define TD "/data/data/com.termux/files/home/tmp/pe_branch_test"

int main(void) {
    system("rm -rf " TD);
    mkdir(TD, 0755);

    /* Two files: main.c (both branches touch) + util.c (only main touches) */
    FILE *f = fopen(TD "/main.c", "w");
    fprintf(f, "// main.c\nint main() {\n    return 0;\n}\n");
    fclose(f);
    f = fopen(TD "/util.c", "w");
    fprintf(f, "// util.c\nint add(int a, int b) {\n    return a + b;\n}\n");
    fclose(f);

    pe_t *pe = pe_create(TD, 1);
    assert(pe);
    assert(pe_cache_file(pe, "main.c") == 0);
    assert(pe_cache_file(pe, "util.c") == 0);

    /* ── Create branches ── */
    assert(pe_branch_create(pe, "feat") == 0);
    assert(pe_branch_create(pe, "conflict") == 0);
    assert(strcmp(pe_branch_current(pe), "main") == 0);

    /* ── Edit on main: add a new file + modify util.c ── */
    /* New file created via cache-then-insert (cache injects empty file) */
    f = fopen(TD "/main_only.c", "w");
    fprintf(f, "// main-only file\n");
    fclose(f);
    assert(pe_cache_file(pe, "main_only.c") == 0);

    pe_edit_t *e = pe_edit_create(pe, "util.c", PE_OP_REPLACE,
        (pe_pos_t){2, 1}, (pe_pos_t){3, 1},
        "int add(int a, int b) {  // from main\n    return a + b + 1;\n", 57);
    assert(e);
    assert(pe_edit_submit(pe, e) == 0);
    assert(pe_flush(pe) == 0);

    size_t sz; const char *d;
    d = pe_file_data(pe, "main.c", &sz);
    assert(d && strstr(d, "return 0;") != NULL);  /* main.c unchanged on main */
    d = pe_file_data(pe, "util.c", &sz);
    assert(d && strstr(d, "from main") != NULL);

    /* ── Switch to feat, make different edits ── */
    assert(pe_branch_switch(pe, "feat") == 0);
    assert(strcmp(pe_branch_current(pe), "feat") == 0);

    /* feat doesn't have main_only.c or the util.c change */
    d = pe_file_data(pe, "util.c", &sz);
    assert(d && strstr(d, "from main") == NULL);
    assert(d && strstr(d, "return a + b;") != NULL);

    /* Edit main.c on feat (line-based replace on same line as original) */
    e = pe_edit_create(pe, "main.c", PE_OP_REPLACE,
        (pe_pos_t){1, 1}, (pe_pos_t){2, 1},
        "// main.c — from feat\n", 22);
    assert(e);
    assert(pe_edit_submit(pe, e) == 0);
    assert(pe_flush(pe) == 0);
    d = pe_file_data(pe, "main.c", &sz);
    assert(d && strstr(d, "from feat") != NULL);

    /* ── List branches ── */
    char **names;
    size_t n = pe_branch_list(pe, &names);
    assert(n >= 4);  /* main, feat, conflict + implicit "main" */
    int found_main = 0, found_feat = 0, found_conflict = 0;
    for (size_t i = 0; i < n; i++) {
        if (strcmp(names[i], "main") == 0) found_main = 1;
        if (strcmp(names[i], "feat") == 0) found_feat = 1;
        if (strcmp(names[i], "conflict") == 0) found_conflict = 1;
        free(names[i]);
    }
    free(names);
    assert(found_main && found_feat && found_conflict);

    /* ── Merge feat into main ──
       feat: changed main.c line 1, didn't touch util.c
       main: didn't touch main.c, changed util.c, added main_only.c
       → non-conflicting: main.c takes feat's version, util.c stays, main_only.c stays */
    assert(pe_branch_merge(pe, "feat", "main") == 0);

    /* Switch to main, verify merge */
    assert(pe_branch_switch(pe, "main") == 0);
    d = pe_file_data(pe, "main.c", &sz);
    assert(d && strstr(d, "from feat") != NULL);      /* feat's change merged in */
    d = pe_file_data(pe, "util.c", &sz);
    assert(d && strstr(d, "from main") != NULL);       /* main's change preserved */
    d = pe_file_data(pe, "main_only.c", &sz);
    assert(d && strstr(d, "main-only") != NULL);       /* main's new file kept */

    /* ── Conflict test: both branches modify same line of same file ── */
    assert(pe_branch_switch(pe, "conflict") == 0);
    e = pe_edit_create(pe, "main.c", PE_OP_REPLACE,
        (pe_pos_t){1, 1}, (pe_pos_t){2, 1},
        "// CONFLICT version\n", 20);
    assert(e);
    assert(pe_edit_submit(pe, e) == 0);
    assert(pe_flush(pe) == 0);

    int rc = pe_branch_merge(pe, "conflict", "main");
    assert(rc == 1);  /* conflicts expected */
    assert(pe_branch_switch(pe, "main") == 0);
    d = pe_file_data(pe, "main.c", &sz);
    assert(d && strstr(d, "<<<<<<< A") != NULL);
    assert(d && strstr(d, ">>>>>>> B") != NULL);

    /* ── Merge feat's new file test ── */
    assert(pe_branch_switch(pe, "feat") == 0);
    /* Create a file only on feat */
    f = fopen(TD "/feat_only.c", "w");
    fprintf(f, "// feat-only file\n");
    fclose(f);
    assert(pe_cache_file(pe, "feat_only.c") == 0);
    assert(pe_branch_merge(pe, "feat", "main") == 0);

    assert(pe_branch_switch(pe, "main") == 0);
    assert(pe_cache_file(pe, "feat_only.c") == 0);  /* should now exist on main */
    d = pe_file_data(pe, "feat_only.c", &sz);
    assert(d && strstr(d, "feat-only") != NULL);

    /* ── Delete branches ── */
    assert(pe_branch_delete(pe, "feat") == 0);
    assert(pe_branch_delete(pe, "conflict") == 0);
    assert(pe_branch_delete(pe, "main") == -1);  /* can't delete current */

    pe_destroy(pe);
    system("rm -rf " TD);
    printf("test_branch: all tests passed\n");
    return 0;
}
