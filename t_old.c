/* t_old.c — exposes the v3 (pre-reassociation) hybrid as old_hash() */
#define plumshash_hybrid plumshash_hybrid_v3
#define PLUMSHASH_HYBRID_IMPLEMENTATION
#include "plumshash_hybrid_v3.h"
#undef plumshash_hybrid
#include <stddef.h>
#include <stdint.h>
uint64_t old_hash(const void *b, size_t l, uint64_t s) { return plumshash_hybrid_v3(b, l, s); }
