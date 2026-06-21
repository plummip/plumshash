/*
 * fractal_hash.h — Dual-Ring Hash Module (Z/7Z × Z/9Z)
 * ======================================================
 *
 * Z/7Z (field):  perfect permutations, no nilpotents, smooth mixing.
 *                Used for primary hash distribution (route8, bloom).
 *
 * Z/9Z (broken): nilpotent steer {3,6,9}, 3-lane fibration.
 *                Used for fuzzy search expansion (Phase 2).
 *
 * The dual ring Z/63Z ≅ Z/7Z × Z/9Z (by CRT) combines:
 *   flow (Z/7Z) — everything permutes, nothing sticks
 *   steer (Z/9Z) — nilpotent attractors, lane structure
 *
 * Factor 11 is a generator of (Z/63Z)× with order 6:
 *   11 mod 7 = 4 (order 3 in Z/7Z)
 *   11 mod 9 = 2 (order 6 in Z/9Z)
 *   Combined order = lcm(3,6) = 6  ← matches our 8-depth requirement
 */

#ifndef FRACTAL_HASH_H
#define FRACTAL_HASH_H

#include <stdint.h>
#include <stddef.h>
#include "fractal_portable.h"

#define HW_CRC32(v) fp_crc32c_u64(v)

/* ═══════════════════════════════════════════════════════════
 * RING PRIMITIVES
 * ═══════════════════════════════════════════════════════════ */

/* Z/7Z — the flow ring (field) */
static inline uint8_t dr7(uint32_t v) {
    uint8_t r = v % 7;
    return r ? r : 7;
}
static inline uint8_t key_dr7(const uint8_t *k, size_t n) {
    uint32_t s = 0;
    for (size_t i = 0; i < n; i++) s += k[i];
    return dr7(s);
}

/* Z/9Z — the steer ring (broken) */
static inline uint8_t dr9(uint32_t v) {
    uint8_t r = v % 9;
    return r ? r : 9;
}
static inline uint8_t key_dr9(const uint8_t *k, size_t n) {
    uint32_t s = 0;
    for (size_t i = 0; i < n; i++) s += k[i];
    return dr9(s);
}

/* Lane classifier (Z/9Z → Z/3Z fibration) */
static inline uint8_t lane_of(uint8_t dr) {
    return (dr - 1) / 3;  /* {1,2,3}→0  {4,5,6}→1  {7,8,9}→2 */
}

/* Nilpotent test */
static inline int is_nilpotent(uint8_t dr) {
    return dr == 3 || dr == 6 || dr == 9;
}

/* ═══════════════════════════════════════════════════════════
 * SPLITMIX64 — golden ratio finalizer
 * ═══════════════════════════════════════════════════════════ */

static inline uint64_t splitmix64(uint64_t x) {
    x ^= x >> 30;
    x *= 0xBF58476D1CE4E5B9ULL;
    x ^= x >> 27;
    x *= 0x94D049BB133111EBULL;
    x ^= x >> 31;
    return x;
}

/* Hash a byte sequence into a full-entropy 64-bit word.
 * FNV-1a accumulation → splitmix64 finalizer.
 * The golden ratio φ ≡ 7 mod 9 and φ ≡ ? mod 7 ties both rings. */
static inline uint64_t hash64(const uint8_t *key, size_t len) {
    uint64_t h = 0x9E3779B97F4A7C15ULL;  /* φ⁻¹·2⁶⁴ */
    for (size_t i = 0; i < len; i++) {
        h ^= (uint64_t)key[i];
        h *= 0x9E3779B97F4A7C15ULL;
    }
    h ^= (uint64_t)len;
    return splitmix64(h);
}

/* ═══════════════════════════════════════════════════════════
 * FRACTAL ROTATION — factor 11
 * ═══════════════════════════════════════════════════════════
 *
 * 11 is a generator of (Z/63Z)× with order 6:
 *   11 mod 7 = 4  → order 3 in Z/7Z×
 *   11 mod 9 = 2  → order 6 in Z/9Z×
 *   lcm(3,6) = 6  → walks full 6-cycle in combined ring
 *
 * At depth d, rotation = (11·d) mod 64.
 * Mod-7 trace: {0,4,1,5,2,6,3,0}  — visits all 7 classes
 * Mod-9 trace: {0,2,4,6,8,1,3,5}  — visits 8 of 9 classes
 */

#define ROT_FACTOR 11

static inline uint64_t frot(uint64_t x, size_t depth) {
    uint32_t r = (ROT_FACTOR * (uint32_t)depth) & 63;
    if (r == 0) return x;
    return (x >> r) | (x << (64 - r));
}

/* ═══════════════════════════════════════════════════════════
 * DUAL-RING TAG ENCODING
 * ═══════════════════════════════════════════════════════════
 *
 *   bits  0–27 : CRC32-C of key prefix (28 bits, hardware)
 *   bits 28–30 : Z/7Z digital root (3 bits, values 1–7)
 *   bits 32–35 : Z/9Z digital root (4 bits, values 1–9)
 *   bits 36–41 : length (6 bits, 0–63)
 *   bit  42    : tail flag
 *   bits 43–63 : reserved
 *
 * The dual dr enables:
 *   - Z/7Z for exact-match discrimination (field property)
 *   - Z/9Z for fuzzy neighbor expansion (broken ring)
 *   - Independent bit positions for SIMD-masked comparison
 */

static inline uint64_t encode_tag(const uint8_t *key, size_t len) {
    uint64_t tag = 0;

    /* CRC32-C of first 8 bytes (zero-padded) */
    uint64_t prefix = 0;
    for (size_t i = 0; i < 8 && i < len; i++)
        prefix |= ((uint64_t)key[i]) << (i * 8);
    uint32_t crc = HW_CRC32(prefix);
    tag |= (uint64_t)(crc & 0x0FFFFFFF);              /* bits 0–27 */

    tag |= ((uint64_t)key_dr7(key, len) & 0x7) << 28; /* bits 28–30 */
    tag |= ((uint64_t)key_dr9(key, len) & 0xF) << 32; /* bits 32–35 */
    tag |= ((uint64_t)(len & 0x3F)) << 36;             /* bits 36–41 */
    tag |= (len > 7 ? 1ULL : 0) << 42;                 /* bit 42 */

    return tag;
}

static inline uint8_t tag_dr7(uint64_t tag) { return (tag >> 28) & 0x7; }
static inline uint8_t tag_dr9(uint64_t tag) { return (tag >> 32) & 0xF; }

/* ═══════════════════════════════════════════════════════════
 * ROUTING (route8)
 * ═══════════════════════════════════════════════════════════
 *
 * hash64(key) → frot(depth) → CRC32-C → {page, slot}
 * The Z/7Z field property ensures uniform page distribution.
 * The Z/9Z lane structure creates self-similar routing.
 */

#define PAGE_COUNT  65536
#define CANDIDATES  8

static void route8(const uint8_t *key, size_t len,
                   uint32_t pages[CANDIDATES], uint8_t slots[CANDIDATES]) {
    uint64_t base = hash64(key, len);
    for (int i = 0; i < CANDIDATES; i++) {
        uint64_t rot  = frot(base, i);
        uint32_t crc  = HW_CRC32(rot);
        pages[i] = crc & 0xFFFF;
        slots[i] = (crc >> 16) & 0x3F;
    }
}

/* ═══════════════════════════════════════════════════════════
 * NILPOTENT-AWARE SCORING
 * ═══════════════════════════════════════════════════════════
 *
 * When choosing a cuckoo displacement victim, prefer entries
 * with nilpotent Z/9Z digital root. These are structurally
 * unstable and redistribute naturally.
 *
 *   score = 2  if dr9 ∈ {3,6,9}  (nilpotent — most unstable)
 *   score = 1  if dr9 ∈ {1,4,7}  (quadratic residue in Z/9Z)
 *   score = 0  if dr9 ∈ {2,5,8}  (quadratic non-residue)
 */

static inline int nilpotent_score(uint64_t tag) {
    uint8_t d = tag_dr9(tag);
    if (is_nilpotent(d)) return 2;
    if (d == 1 || d == 4 || d == 7) return 1;
    return 0;
}

/* ═══════════════════════════════════════════════════════════
 * 3-LANE FUZZY PHASE 2 BOUND
 * ═══════════════════════════════════════════════════════════
 *
 * Instead of probing all 2k+1 residue classes in Z/9Z,
 * probe only lane_delta lanes in the fibration Z/9Z → Z/3Z.
 *
 *   max_edit=1 → 1 lane  (3 DRs, 3× smaller search)
 *   max_edit=2 → 2 lanes (6 DRs)
 *   max_edit=3 → 3 lanes (9 DRs = full scan)
 */

static inline int lane_delta_bound(int max_edit) {
    return (max_edit + 2) / 3;  /* ceil(max_edit/3) */
}

/* Ring distance in Z/9Z: min(|a-b|, 9-|a-b|) */
static inline int ring_dist(uint8_t a, uint8_t b) {
    int d = (int)a - (int)b;
    if (d < 0) d = -d;
    return d < 9 - d ? d : 9 - d;
}

/* ═══════════════════════════════════════════════════════════
 * FRACTAL DISTANCE (multi-scale)
 * ═══════════════════════════════════════════════════════════
 *
 * Measures divergence at 8 depth scales.
 * Each depth compares the Z/9Z digital root of 8-byte segments.
 * edit_distance(a,b) ≤ k  ⇒  fractal_distance(a,b) ≤ k
 */

static inline uint64_t seg_at(const uint8_t *s, size_t len, size_t d) {
    size_t off = d * 4;
    uint64_t seg = 0;
    for (size_t i = 0; i < 8; i++) {
        size_t pos = off + i;
        if (pos < len) seg |= ((uint64_t)s[pos]) << (i * 8);
    }
    return seg;
}

static double fractal_dist(const uint8_t *a, size_t la,
                            const uint8_t *b, size_t lb) {
    int total = 0;
    for (int d = 0; d < 8; d++) {
        uint8_t da = dr9(seg_at(a, la, d));
        uint8_t db = dr9(seg_at(b, lb, d));
        total += ring_dist(da, db);
    }
    return (double)total / 8.0;
}

#endif /* FRACTAL_HASH_H */
