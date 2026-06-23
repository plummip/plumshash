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

    FILE *f = fopen(TD "/main.c", "w");
    fprintf(f, "// main.c\nint main() {\n    return 0;\n}\n");
    fclose(f);

    pe_t *pe = pe_create(TD, 1);
    assert(pe);
    assert(pe_cache_file(pe, "main.c") == 0);

    /* Branch create */
    printf("1. branch_create: %d\n", pe_branch_create(pe, "feat"));

    /* Edit on main */
    pe_edit_t *e = pe_edit_create(pe, "main.c", PE_OP_INSERT,
        (pe_pos_t){1,1}, (pe_pos_t){0,0}, "// main edit\n", 13);
    assert(e);
    pe_edit_submit(pe, e);
    pe_flush(pe);

    size_t sz;
    const char *d = pe_file_data(pe, "main.c", &sz);
    printf("2. main after edit: %.40s\n", d);

    /* Switch to feat */
    printf("3. branch_switch: %d\n", pe_branch_switch(pe, "feat"));
    printf("4. current: %s\n", pe_branch_current(pe));

    d = pe_file_data(pe, "main.c", &sz);
    printf("5. feat main.c: %.40s\n", d);

    /* Edit on feat */
    e = pe_edit_create(pe, "main.c", PE_OP_INSERT,
        (pe_pos_t){1,1}, (pe_pos_t){0,0}, "// feat edit\n", 12);
    assert(e);
    pe_edit_submit(pe, e);
    pe_flush(pe);
    d = pe_file_data(pe, "main.c", &sz);
    printf("6. feat after edit: %.50s\n", d);

    /* List branches */
    char **names;
    size_t n = pe_branch_list(pe, &names);
    printf("7. branches (%zu): ", n);
    for (size_t i = 0; i < n; i++) { printf("%s ", names[i]); free(names[i]); }
    free(names);
    printf("\n");

    /* Merge feat into main */
    printf("8. merge: %d\n", pe_branch_merge(pe, "feat", "main"));

    /* Switch to main, check */
    pe_branch_switch(pe, "main");
    d = pe_file_data(pe, "main.c", &sz);
    printf("9. main after merge: %.60s\n", d);

    pe_destroy(pe);
    system("rm -rf " TD);
    printf("PASS\n");
    return 0;
}
