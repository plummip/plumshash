/*
 * test_hybrid_security.c — focused safety regressions for hybrid_intern.h.
 *
 * These tests exercise malformed handles, metadata boundaries, oversized
 * lengths, and the AVX2 comparison tail. The file is compiled with ASan and
 * UBSan by the repository test gate.
 */
#define PLUMSHASH_IMPLEMENTATION
#include "hybrid_intern.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#if defined(__linux__)
#include <sys/mman.h>
#endif

static int passed = 0;
static int failed = 0;

#define CHECK(name, condition) do { \
    if (condition) { \
        passed++; \
        printf("  PASS: %s\n", name); \
    } else { \
        failed++; \
        printf("  FAIL: %s\n", name); \
    } \
} while (0)

static void test_large_length_roundtrip(void) {
    const uint32_t len = 65536;
    uint8_t *src = (uint8_t *)malloc(len);
    if (!src) {
        failed++;
        printf("  FAIL: large_length_alloc\n");
        return;
    }
    for (uint32_t i = 0; i < len; i++) src[i] = (uint8_t)(i * 37u + 11u);

    HybridPool *hp = hybrid_create(1, 64, 64);
    SF_Handle h1 = hybrid_intern(hp, (const char *)src, len);
    SF_Handle h2 = hybrid_intern(hp, (const char *)src, len);
    uint32_t resolved_len = 0;
    const char *resolved = hybrid_str(hp, h1, &resolved_len);

    CHECK("large_length_handle", h1.u != 0);
    CHECK("large_length_dedup", h1.u == h2.u);
    CHECK("large_length_roundtrip",
          resolved != NULL && resolved_len == len &&
          memcmp(resolved, src, len) == 0);

    hybrid_destroy(hp);
    free(src);
}

static void test_forged_rank(void) {
    pid_t pid = fork();
    if (pid < 0) {
        failed++;
        printf("  FAIL: forged_rank_fork\n");
        return;
    }
    if (pid == 0) {
        HybridPool *hp = hybrid_create(2, 64, 64);
        Pool *p = hp->short_pool;
        /* A location handle whose chunk index is outside the arena. */
        SF_Handle forged = _encode_handle(p->pool_id, p->epoch[1], 1,
                                          (uint16_t)(IR_CHUNK_TIER | 0x3FFu),
                                          0);
        if (hybrid_str(hp, forged, NULL) != NULL) _exit(2);
        hybrid_evict(hp, forged); /* must not index outside the arena */
        _exit(0);
    }

    int status = 0;
    if (waitpid(pid, &status, 0) != pid) {
        failed++;
        printf("  FAIL: forged_rank_wait\n");
        return;
    }
    CHECK("forged_rank_rejected",
          WIFEXITED(status) && WEXITSTATUS(status) == 0);
}

static void test_tier_mismatch(void) {
    HybridPool *hp = hybrid_create(3, 64, 64);
    SF_Handle stable = hybrid_intern(hp, "stable-tier", 11);
    /* Flip the tier bit of the chunk id: the handle then names a location
     * in the other tier's arena. */
    uint16_t chunk = (uint16_t)(_handle_chunk(stable) ^ IR_CHUNK_TIER);
    SF_Handle forged = _encode_handle(_handle_pool(stable), _handle_epoch(stable),
                                      _handle_gen(stable), chunk,
                                      _handle_offset(stable));

    CHECK("tier_mismatch_resolve", hybrid_str(hp, forged, NULL) == NULL);

    hybrid_evict(hp, forged);
    CHECK("tier_mismatch_no_evict", hybrid_str(hp, stable, NULL) != NULL);

    hybrid_destroy(hp);
}

static void test_free_slot_resolution(void) {
    HybridPool *hp = hybrid_create(4, 64, 64);
    SF_Handle stable = hybrid_intern(hp, "free-slot", 9);
    Pool *p = hp->short_pool;
    uint16_t pool_id = _handle_pool(stable);

    hybrid_evict(hp, stable);

    /* The eviction bumps the LOCATION's generation, so the handle that
     * pointed at it stops resolving. */
    CHECK("free_slot_resolve", hybrid_str(hp, stable, NULL) == NULL);

    /* A handle forged with the location's CURRENT generation resolves to
     * the bytes that are still there: the bytes are only invalidated, never
     * erased. */
    uint16_t chunk = _handle_chunk(stable);
    uint8_t index = (uint8_t)(chunk & IR_CHUNK_INDEX);
    const IpPrefix *pfx = (const IpPrefix *)(p->t1.chunks[index]->data +
                                             _handle_offset(stable));
    SF_Handle forged = _encode_handle(pool_id, _handle_epoch(stable),
                                      (uint16_t)IP_PREFIX_GEN(pfx), chunk,
                                      _handle_offset(stable));
    CHECK("evicted_bytes_still_readable", hybrid_str(hp, forged, NULL) != NULL);

    hybrid_destroy(hp);
}

static void test_oversized_length_rejected(void) {
    pid_t pid = fork();
    if (pid < 0) {
        failed++;
        printf("  FAIL: oversized_length_fork\n");
        return;
    }
    if (pid == 0) {
        HybridPool *hp = hybrid_create(5, 64, 64);
        SF_Handle h = hybrid_intern(hp, "x", UINT32_MAX);
        _exit(h.u == 0 ? 0 : 2);
    }

    int status = 0;
    if (waitpid(pid, &status, 0) != pid) {
        failed++;
        printf("  FAIL: oversized_length_wait\n");
        return;
    }
    CHECK("oversized_length_rejected",
          WIFEXITED(status) && WEXITSTATUS(status) == 0);
}

static void test_invalid_pool_id(void) {
    HybridPool *hp = hybrid_create(5000, 64, 64);
    CHECK("invalid_pool_id_rejected", hp == NULL);
    if (hp) hybrid_destroy(hp);
}

static void test_invalid_size_hints(void) {
    pid_t pid = fork();
    if (pid < 0) {
        failed++;
        printf("  FAIL: invalid_size_hints_fork\n");
        return;
    }
    if (pid == 0) {
        HybridPool *hp = hybrid_create(1, UINT32_MAX, 64);
        if (hp) {
            hybrid_destroy(hp);
            _exit(2);
        }
        _exit(0);
    }

    int status = 0;
    if (waitpid(pid, &status, 0) != pid) {
        failed++;
        printf("  FAIL: invalid_size_hints_wait\n");
        return;
    }
    CHECK("invalid_size_hints_rejected",
          WIFEXITED(status) && WEXITSTATUS(status) == 0);
}

#if defined(__AVX2__) && defined(__linux__)
static void test_avx2_tail_bounds(void) {
    const size_t page = (size_t)sysconf(_SC_PAGESIZE);
    uint8_t *map = (uint8_t *)mmap(NULL, page * 2, PROT_READ | PROT_WRITE,
                                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (map == MAP_FAILED) {
        failed++;
        printf("  FAIL: avx2_tail_mmap\n");
        return;
    }
    if (mprotect(map + page, page, PROT_NONE) != 0) {
        munmap(map, page * 2);
        failed++;
        printf("  FAIL: avx2_tail_mprotect\n");
        return;
    }

    const uint32_t len = 33;
    char *src = (char *)(map + page - len);
    for (uint32_t i = 0; i < len; i++) src[i] = (char)('a' + (i % 26));

    HybridPool *hp = hybrid_create(6, 64, 64);
    SF_Handle h1 = hybrid_intern(hp, src, len);
    SF_Handle h2 = hybrid_intern(hp, src, len);
    CHECK("avx2_tail_dedup", h1.u != 0 && h1.u == h2.u);

    hybrid_destroy(hp);
    munmap(map, page * 2);
}
#else
static void test_avx2_tail_bounds(void) {
    printf("  SKIP: avx2_tail_bounds (requires AVX2 + Linux)\n");
}
#endif

int main(void) {
    printf("Hybrid intern security regressions\n");
    test_large_length_roundtrip();
    test_forged_rank();
    test_tier_mismatch();
    test_free_slot_resolution();
    test_oversized_length_rejected();
    test_invalid_pool_id();
    test_invalid_size_hints();
    test_avx2_tail_bounds();

    printf("Results: %d passed, %d failed\n", passed, failed);
    return failed ? 1 : 0;
}
