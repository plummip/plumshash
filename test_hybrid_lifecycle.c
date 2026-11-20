/*
 * test_hybrid_lifecycle.c — slot lifecycle (evict / epoch / reuse) for
 * hybrid_intern.h.
 *
 * The intern pool is an open-addressed table with linear probing. Deleting a
 * slot has to leave the probe chain intact, otherwise a lookup for a string
 * that lives further down the chain stops at the hole and the re-intern
 * inserts a DUPLICATE instead of resolving the existing entry. These tests
 * pin the tombstone model:
 *
 *   1. evict_intern_churn       an evict+intern loop never exhausts the
 *                               table (a freed slot is reusable) and the
 *                               reported live count stays truthful.
 *   2. dedup_survives_eviction  a string behind a deleted slot is still
 *                               found: same handle back, no duplicate entry.
 *   3. scratch_epoch_churn      the scratch tier keeps working across epoch
 *                               boundaries (deleted slots are reclaimed).
 *   4. stale_handles_null       evicted / epoch-cleared handles resolve NULL.
 *   5. tombstone_reuse          the next insert on a chain reuses the
 *                               deleted slot instead of growing the table.
 *
 * Built with ASan + UBSan by the repository test gate.
 */
#define PLUMSHASH_IMPLEMENTATION
#include "hybrid_intern.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

/* The tombstone counters only exist from hybrid_intern 1.1 on; against an
 * older header the counter assertions are skipped, the behavioural ones
 * still run (and fail, which is the point of the regression). */
#ifndef HYBRID_INTERN_HAS_TOMBSTONES
# define HYBRID_INTERN_HAS_TOMBSTONES 0
#endif

/* The table slot that holds a string. The handle names the LOCATION, not
 * the slot, so the slot index comes from the arena prefix. */
static uint32_t slot_of(HybridPool *hp, SF_Handle h) {
    Pool *p = (_handle_pool(h) == hp->short_pool->pool_id) ? hp->short_pool
                                                            : hp->long_pool;
    uint16_t chunk = _handle_chunk(h);
    uint8_t tier = (chunk & IR_CHUNK_TIER) ? TIER_T1 : TIER_T0;
    uint8_t index = (uint8_t)(chunk & IR_CHUNK_INDEX);
    const Arena *a = tier == TIER_T1 ? &p->t1 : &p->t0;
    if (index >= a->n_chunks) return UINT32_MAX;
    const IpPrefix *pfx = (const IpPrefix *)(a->chunks[index]->data +
                                             _handle_offset(h));
    return IP_PREFIX_SLOT(pfx);
}

/* Home slot of a string in the short pool — the index derivation is
 * hash & (M - 1), so the seed decides which chain a string lands on. */
static uint32_t home_of(HybridPool *hp, const char *s, uint32_t len) {
    Pool *sp = hp->short_pool;
    return (uint32_t)plumshash(s, len, sp->seed) & (sp->M - 1);
}

/* Find a second, different string that hashes into the same chain. */
static int find_chain_partner(HybridPool *hp, const char *first,
                               uint32_t flen, char *out, uint32_t outcap,
                               uint32_t *outlen) {
    uint32_t home = home_of(hp, first, flen);
    for (int i = 0; i < 200000; i++) {
        int n = snprintf(out, outcap, "chain-partner-%d", i);
        if (n <= 0) return 0;
        uint32_t len = (uint32_t)n;
        if (memcmp(out, first, len < flen ? len : flen) == 0) continue;
        if (home_of(hp, out, len) == home) { *outlen = len; return 1; }
    }
    return 0;
}

static void test_evict_intern_churn(void) {
    HYBRID_SCOPED(hp, 11, 64, 64);              /* M = 128 slots */
    uint32_t M = hp->short_pool->M;
    char buf[32];
    int first_fail = -1;
    uint32_t iters = M * 4;

    for (uint32_t i = 0; i < iters; i++) {
        int n = snprintf(buf, sizeof buf, "churn-%u", i);
        SF_Handle h = hybrid_intern(hp, buf, (uint32_t)n);
        if (h.u == 0) { first_fail = (int)i; break; }
        hybrid_evict(hp, h);
    }
    CHECK("evict_intern_churn_no_exhaustion", first_fail < 0);

    /* every entry was evicted again: the live count must be back to zero */
    HybridStats st = hybrid_stats(hp);
    CHECK("evict_intern_churn_live_count", st.short_entries == 0);

    /* the deleted slots are reusable: the table still accepts inserts */
    SF_Handle h = hybrid_intern(hp, "after-churn", 11);
    CHECK("evict_intern_churn_still_usable", h.u != 0);
    HybridStats st2 = hybrid_stats(hp);
    CHECK("evict_intern_churn_stats_honest", st2.short_entries == 1);
}

static void test_dedup_survives_eviction(void) {
    HYBRID_SCOPED(hp, 12, 64, 64);
    char partner[64];
    uint32_t plen = 0;
    const char *a = "chain-head";
    uint32_t alen = (uint32_t)strlen(a);

    CHECK("dedup_chain_partner_found",
          find_chain_partner(hp, a, alen, partner, sizeof partner, &plen));

    SF_Handle ha = hybrid_intern(hp, a, alen);
    SF_Handle hb = hybrid_intern(hp, partner, plen);
    CHECK("dedup_both_interned", ha.u != 0 && hb.u != 0);
    CHECK("dedup_chain_shared", slot_of(hp, ha) != slot_of(hp, hb));

    /* delete the head of the chain; the second string must still resolve */
    hybrid_evict(hp, ha);
    SF_Handle hb2 = hybrid_intern(hp, partner, plen);
    CHECK("dedup_survives_eviction_same_handle", hb2.u == hb.u);

    /* and it must not have created a second copy */
    HybridStats st = hybrid_stats(hp);
    CHECK("dedup_survives_eviction_no_duplicate", st.short_entries == 1);
}

static void test_scratch_epoch_churn(void) {
    HYBRID_SCOPED(hp, 13, 64, 64);
    uint32_t M = hp->short_pool->M;
    char buf[32];
    int first_fail = -1;

    for (uint32_t i = 0; i < M; i++) {
        int n = snprintf(buf, sizeof buf, "scratch-%u", i);
        SF_Handle h = hybrid_intern_scratch(hp, buf, (uint32_t)n);
        if (h.u == 0) { first_fail = (int)i; break; }
        hybrid_epoch_end(hp);
    }
    CHECK("scratch_epoch_churn_no_exhaustion", first_fail < 0);

    HybridStats st = hybrid_stats(hp);
    CHECK("scratch_epoch_churn_cleared", st.short_entries == 0);
}

static void test_stale_handles_null(void) {
    HYBRID_SCOPED(hp, 14, 64, 64);
    const char *k = "stale-handle";
    SF_Handle h = hybrid_intern(hp, k, (uint32_t)strlen(k));
    CHECK("stale_handle_interned", h.u != 0);
    hybrid_evict(hp, h);
    CHECK("stale_handle_after_evict_null", hybrid_str(hp, h, NULL) == NULL);

    const char *sk = "stale-scratch";
    SF_Handle sh = hybrid_intern_scratch(hp, sk, (uint32_t)strlen(sk));
    CHECK("stale_scratch_interned", sh.u != 0);
    hybrid_epoch_end(hp);
    CHECK("stale_scratch_after_epoch_null", hybrid_str(hp, sh, NULL) == NULL);
}

static void test_tombstone_reuse(void) {
    HYBRID_SCOPED(hp, 15, 64, 64);
    const char *a = "reuse-head";
    uint32_t alen = (uint32_t)strlen(a);
    char partner[64];
    uint32_t plen = 0;
    if (!find_chain_partner(hp, a, alen, partner, sizeof partner, &plen)) {
        failed++;
        printf("  FAIL: tombstone_reuse_partner_found\n");
        return;
    }
    /* keep the partner live: the chain head is deleted while the rest of
     * the chain stays occupied, which is exactly the case a naive free
     * would break */
    SF_Handle hb = hybrid_intern(hp, partner, plen);
    CHECK("tombstone_reuse_partner_interned", hb.u != 0);

    SF_Handle ha = hybrid_intern(hp, a, alen);
    uint32_t slot = slot_of(hp, ha);
    hybrid_evict(hp, ha);                          /* delete the chain head */

    HybridStats st1 = hybrid_stats(hp);
#if HYBRID_INTERN_HAS_TOMBSTONES
    CHECK("tombstone_counted", st1.short_tombs >= 1);
#endif

    /* a new string on the same chain reuses the deleted slot */
    char probe[64];
    uint32_t probe_len = 0;
    int found = 0;
    for (int i = 0; i < 200000 && !found; i++) {
        int n = snprintf(probe, sizeof probe, "reuse-%d", i);
        if (home_of(hp, probe, (uint32_t)n) == slot) { probe_len = (uint32_t)n; found = 1; }
    }
    if (!found) {
        failed++;
        printf("  FAIL: tombstone_reuse_probe_found\n");
        return;
    }
    SF_Handle hn = hybrid_intern(hp, probe, probe_len);
    CHECK("tombstone_reuse_interned", hn.u != 0);
    CHECK("tombstone_reuse_same_slot", slot_of(hp, hn) == slot);
    HybridStats st2 = hybrid_stats(hp);
#if HYBRID_INTERN_HAS_TOMBSTONES
    CHECK("tombstone_reuse_count_dropped", st2.short_tombs < st1.short_tombs);
#else
    (void)st1; (void)st2;
#endif
}

/* The table can grow: a handle names an arena LOCATION, not a slot, so the
 * entries may be re-placed anywhere. Growing re-hashes every live entry and
 * re-places it; the strings and the handles must survive that untouched. */
static void test_table_grows(void) {
    enum { N = 5000 };
    static char buf[N][24];
    static SF_Handle h[N];

    HYBRID_SCOPED(hp, 16, 64, 64);          /* M = 128 slots to start */
    uint32_t m0 = hp->short_pool->M;

    int all = 1;
    for (int i = 0; i < N; i++) {
        snprintf(buf[i], sizeof buf[i], "grow-%d", i);
        h[i] = hybrid_intern(hp, buf[i], (uint32_t)strlen(buf[i]));
        if (h[i].u == 0) { all = 0; break; }
    }
    CHECK("grow_all_interned", all);
    CHECK("grow_table_doubled", hp->short_pool->M > m0);

    int ok = 1;
    for (int i = 0; i < N; i++) {
        uint32_t len = 0;
        const char *s = hybrid_str(hp, h[i], &len);
        if (!s || len != strlen(buf[i]) || memcmp(s, buf[i], len) != 0) { ok = 0; break; }
    }
    CHECK("grow_handles_still_resolve", ok);

    int dedup = 1;
    for (int i = 0; i < N; i += 97) {
        SF_Handle again = hybrid_intern(hp, buf[i], (uint32_t)strlen(buf[i]));
        if (again.u != h[i].u) { dedup = 0; break; }
    }
    CHECK("grow_dedup_intact", dedup);

    /* An eviction after a grow must remove the RIGHT entry: the slot
     * back-pointer in the arena prefix was re-pointed by the grow. */
    hybrid_evict(hp, h[10]);
    CHECK("grow_evict_invalidates", hybrid_str(hp, h[10], NULL) == NULL);
    CHECK("grow_evict_spares_others", hybrid_str(hp, h[11], NULL) != NULL);
    CHECK("grow_evict_spares_neighbours", hybrid_str(hp, h[9], NULL) != NULL);
}

/* Tombstones are reused by later inserts, but a churn-heavy pool can end up
 * with a quarter of its table deleted. The compaction rebuilds the table at
 * the same size with only the live entries — what a backward-shift
 * ("foster") deletion achieves, without shifting entries around: a handle
 * names an arena location, so re-placing an entry invalidates nothing. */
static void test_compaction_purges_tombstones(void) {
    enum { N = 100 };
    static char buf[N][24];
    static SF_Handle h[N];

    HYBRID_SCOPED(hp, 17, 64, 64);          /* M starts at 128 slots */

    for (int i = 0; i < N; i++) {
        snprintf(buf[i], sizeof buf[i], "compact-%d", i);
        h[i] = hybrid_intern(hp, buf[i], (uint32_t)strlen(buf[i]));
    }
    /* the table may have grown while filling; the compaction must not
     * change its size */
    uint32_t m0 = hp->short_pool->M;
    /* delete most of them: the tombstones now outnumber the live entries */
    for (int i = 0; i < N - 5; i++) hybrid_evict(hp, h[i]);
    HybridStats st1 = hybrid_stats(hp);
    CHECK("compaction_tombstones_present", st1.short_tombs > hp->short_pool->M / 4);

    /* one more insert triggers the compaction */
    SF_Handle extra = hybrid_intern(hp, "compact-trigger", 14);
    CHECK("compaction_trigger_interned", extra.u != 0);

    HybridStats st2 = hybrid_stats(hp);
    CHECK("compaction_purged_tombstones", st2.short_tombs < st1.short_tombs);
    CHECK("compaction_kept_size", hp->short_pool->M == m0);

    /* the survivors still resolve, the evicted ones still do not */
    int ok = 1;
    for (int i = N - 5; i < N; i++) {
        uint32_t len = 0;
        const char *s = hybrid_str(hp, h[i], &len);
        if (!s || len != strlen(buf[i]) || memcmp(s, buf[i], len) != 0) { ok = 0; break; }
    }
    CHECK("compaction_survivors_resolve", ok);
    CHECK("compaction_evicted_still_null", hybrid_str(hp, h[0], NULL) == NULL);
}

int main(void) {
    printf("Hybrid intern lifecycle regressions\n");
    test_evict_intern_churn();
    test_dedup_survives_eviction();
    test_scratch_epoch_churn();
    test_stale_handles_null();
    test_tombstone_reuse();
    test_table_grows();
    test_compaction_purges_tombstones();

    printf("Results: %d passed, %d failed\n", passed, failed);
    return failed ? 1 : 0;
}
