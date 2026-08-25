/* t_new.c — exposes the reassociated hybrid as new_hash() */
#define plumshash_hybrid plumshash_hybrid_new
#define PLUMSHASH_HYBRID_IMPLEMENTATION
#include "plumshash_hybrid.h"
#undef plumshash_hybrid
#include <stddef.h>
#include <stdint.h>
uint64_t new_hash(const void *b, size_t l, uint64_t s) { return plumshash_hybrid_new(b, l, s); }
