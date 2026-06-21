#define PLUMSHASH_IMPLEMENTATION
#include "plumshash.h"
#include <stdio.h>
int main() {
    printf("plumshash(\"hello\",5,0) = %016lx\n", plumshash("hello", 5, 0));
    return 0;
}
