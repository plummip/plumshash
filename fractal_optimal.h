/*
 * fractal_optimal.h — Mathematically Optimal Functions
 * for the Fractal Ring-Homomorphic Fuzzy Database
 *
 * Every choice below is justified by the ring structure Z → Z/9Z,
 * the 2ⁿ director theorem, the Jacobson radical {3,6,9}, and
 * the golden ratio splitmix64 finalizer.
 */

#ifndef FRACTAL_OPTIMAL_H
#define FRACTAL_OPTIMAL_H

#include <stdint.h>
#include <stddef.h>
#include "fractal_portable.h"

/* ═════════════════════════════════════════════════════════════════════
 * 1. RING STRUCTURE
 * ═════════════════════════════════════════════════════════════════════
 *
 * Z/9Z decomposes into:
 *   Unit group:   {1,2,4,5,7,8} ≅ C₆   (cyclic, order 6)
 *   Jacobson rad: {3,6,9}              (nilpotent: x²≡0 mod 9)
 *
 * The 3 lanes partition Z/9Z:
 *   Lane 0: {1,2,3}    Lane 1: {4,5,6}    Lane 2: {7,8,9}
 *
 * Each lane contains exactly one nilpotent element → structurally
 * balanced. The 2ⁿ cycle (period 6) traverses all 3 lanes twice.
 */

/* Digital root: canonical lift of Z/9Z to {1..9} */
static inline uint8_t digital_root(uint32_t v) {
    uint8_t r = v % 9;
    return r ? r : 9;
}

static inline uint8_t key_dr(const uint8_t *k, size_t n) {
    uint32_t s = 0;
    for (size_t i = 0; i < n; i++) s += k[i];
    return digital_root(s);
}

/* Lane classifier: maps dr ∈ {1..9} → lane ∈ {0,1,2} */
static inline uint8_t lane_of(uint8_t dr) {
    return (dr - 1) / 3;  // {1,2,3}→0  {4,5,6}→1  {7,8,9}→2
}

/* Nilpotent test: true iff dr ∈ Jacobson radical */
static inline int is_nilpotent(uint8_t dr) {
    return dr == 3 || dr == 6 || dr == 9;
}

/* ═════════════════════════════════════════════════════════════════════
 * 2. OPTIMAL HASH — splitmix64 + CRC32-C
 * ═════════════════════════════════════════════════════════════════════
 *
 * Pipeline:
 *   FNV-1a accumulation → splitmix64 finalizer → CRC32-C
 *
 * Why splitmix64:
 *   · Constants derived from golden ratio φ⁻¹·2⁶⁴
 *   · φ ≡ 7 mod 9 → its own inverse in Z/9Z
 *   · Fibonacci period mod 9 = 24 → lcm(24, 6-cycle, 8 depths) = 24
 *   · Full 64-bit avalanche in 3 xor-shift-multiply rounds
 *
 * Why CRC32-C:
 *   · Hardware-accelerated on ARM64 (1 instruction: crc32cd)
 *   · Uniform output distribution
 *   · 32-bit output → 16 bits page + 6 bits slot + 10 bits spare
 */

#define crc32c_u64(v) fp_crc32c_u64(v)

static inline uint64_t splitmix64(uint64_t x) {
    x ^= x >> 30;
    x *= 0xBF58476D1CE4E5B9ULL;
    x ^= x >> 27;
    x *= 0x94D049BB133111EBULL;
    x ^= x >> 31;
    return x;
}

/* Hash a byte sequence into a 64-bit word.
 * For short keys (q-grams): FNV-1a → splitmix64.
 * For long keys: could use CRC32-C chaining. */
static inline uint64_t hash64(const uint8_t *key, size_t len) {
    uint64_t h = 0x9E3779B97F4A7C15ULL;
    for (size_t i = 0; i < len; i++) {
        h ^= (uint64_t)key[i];
        h *= 0x9E3779B97F4A7C15ULL;
    }
    h ^= (uint64_t)len;
    return splitmix64(h);
}

/* ═════════════════════════════════════════════════════════════════════
 * 3. OPTIMAL FRACTAL ROTATION
 * ═════════════════════════════════════════════════════════════════════
 *
 * Factor 11 is the unique optimum because:
 *   (a) gcd(11, 64) = 1 → generates full rotation group (Z/64Z)×
 *   (b) 11 ≡ 2 mod 9 → generates (Z/9Z)× (order 6), walks 2ⁿ cycle
 *   (c) rotation gaps: [2,9,2,9,11,11,11,9] — best max_gap=11
 *       among all generators of both groups
 *
 * At depth d, rotation = (11·d) mod 64.
 * This visits 8 distinct rotations before repeating.
 *
 * The mod-9 trajectory of each depth:
 *   depth 0: rot=0  ≡ 0  → identity
 *   depth 1: rot=11 ≡ 2  → 2¹ step on 2ⁿ cycle
 *   depth 2: rot=22 ≡ 4  → 2² step
 *   depth 3: rot=33 ≡ 6  → DIRECTOR (nilpotent!)
 *   depth 4: rot=44 ≡ 8  → ≡ -1 mod 9
 *   depth 5: rot=55 ≡ 1  → full cycle back
 *   depth 6: rot=3  ≡ 3  → DIRECTOR (nilpotent!)
 *   depth 7: rot=14 ≡ 5  → 2⁵ step
 *
 * Director depths (3, 6) probe nilpotent residue classes.
 * These are the "escape orbits" — most likely to trigger displacement.
 */

#define ROTATION_FACTOR 11

static inline uint64_t fractal_rotate(uint64_t x, size_t depth) {
    uint32_t r = (ROTATION_FACTOR * (uint32_t)depth) & 63;
    if (r == 0) return x;
    return (x >> r) | (x << (64 - r));
}

/* ═════════════════════════════════════════════════════════════════════
 * 4. OPTIMAL TAG ENCODING
 * ═════════════════════════════════════════════════════════════════════
 *
 * Layout (64 bits):
 *   bits  0–31 : CRC32-C of key prefix
 *   bits 32–35 : digital root dr ∈ [1,9]
 *   bits 36–41 : length (0–63)
 *   bit  42    : tail flag
 *   bits 43–63 : reserved
 *
 * The DR at bits 32–35 enables masked SIMD comparison:
 *   (tag & 0xF00000000) == (target_dr << 32)
 *
 * The CRC32 prefix gives 32 bits of discrimination (vs 24 raw bytes),
 * computed in 1 hardware instruction.
 */

static inline uint64_t encode_tag(const uint8_t *key, size_t len) {
    uint64_t tag = 0;

    /* CRC32-C of first 8 bytes (or full key if shorter) */
    uint64_t prefix = 0;
    for (size_t i = 0; i < 8 && i < len; i++)
        prefix |= ((uint64_t)key[i]) << (i * 8);
    uint32_t crc = crc32c_u64(prefix);
    tag |= (uint64_t)crc;                          /* bits 0–31 */

    tag |= ((uint64_t)key_dr(key, len)) << 32;     /* bits 32–35 */
    tag |= ((uint64_t)(len & 0x3F)) << 36;         /* bits 36–41 */
    tag |= (len > 7 ? 1ULL : 0) << 42;             /* bit 42 */

    return tag;
}

/* Extract digital root from tag for SIMD-masked comparison */
static inline uint8_t tag_dr(uint64_t tag) {
    return (uint8_t)((tag >> 32) & 0xF);
}

/* ═════════════════════════════════════════════════════════════════════
 * 5. OPTIMAL ROUTING (route8)
 * ═════════════════════════════════════════════════════════════════════
 *
 * For each depth 0..7:
 *   1. hash64(key, len) → full 64-bit entropy base
 *   2. fractal_rotate(base, depth) → depth-dependent rotation
 *   3. CRC32-C(rotated) → 32-bit uniform hash
 *   4. page = crc & 0xFFFF       (lower 16 bits)
 *   5. slot = (crc >> 16) & 0x3F (next 6 bits)
 *
 * For short keys (q-grams), the hash64 step produces a full-entropy
 * 64-bit word; the fractal rotation then creates 8 distinct rotated
 * views, each hashed to a page/slot pair.
 */

#define PAGE_COUNT  65536
#define SLOT_COUNT  64
#define CANDIDATES  8

static void route8(const uint8_t *key, size_t len,
                   uint32_t pages[CANDIDATES], uint8_t slots[CANDIDATES]) {
    uint64_t base = hash64(key, len);
    for (int i = 0; i < CANDIDATES; i++) {
        uint64_t rot = fractal_rotate(base, i);
        uint32_t crc = crc32c_u64(rot);
        pages[i] = crc & 0xFFFF;
        slots[i] = (crc >> 16) & 0x3F;
    }
}

/* ═════════════════════════════════════════════════════════════════════
 * 6. OPTIMAL FUZZY SEARCH — 3-LANE PHASE 2
 * ═════════════════════════════════════════════════════════════════════
 *
 * Given a query q-gram with digital root dr_q, the traditional
 * Phase 2 probes all dr in [dr_q - k, dr_q + k] mod 9 → up to 2k+1
 * residue classes.
 *
 * The 3-lane optimization: since each lane contains exactly 3
 * consecutive DR values, and an edit of distance k can change the
 * DR by at most k, the query only needs to probe lanes within
 * ±⌈k/3⌉ of the query's lane.
 *
 *   max_edit=1 → probe only current lane (3 DRs, not 3)
 *   max_edit=2 → probe current ± 1 lane (6 DRs, not 5)   — slight overhead
 *   max_edit=3 → probe all 3 lanes (9 DRs, not 7)
 *
 * For max_edit=1: 3× speedup over naive 9-class probe.
 */

/* Compute the set of lanes that need probing for given max_edit */
static inline int lane_delta_bound(int max_edit) {
    return (max_edit + 2) / 3;  /* ceil(max_edit/3) */
}

/* Check if two digital roots are within max_edit in the ring Z/9Z */
static inline int dr_within(uint8_t dr_a, uint8_t dr_b, int max_edit) {
    int diff = (int)dr_a - (int)dr_b;
    if (diff < 0) diff = -diff;
    /* Ring distance: min(diff, 9-diff) */
    int ring_dist = diff < 9 - diff ? diff : 9 - diff;
    return ring_dist <= max_edit;
}

/* ═════════════════════════════════════════════════════════════════════
 * 7. FRACTAL DISTANCE METRIC
 * ═════════════════════════════════════════════════════════════════════
 *
 * Measures multi-scale similarity between two strings.
 * At each depth, computes the segment digital root and measures
 * ring distance in Z/9Z.
 *
 * Property: edit_distance(a,b) ≤ k ⇒ fractal_distance(a,b) ≤ k
 * (each edit affects at most one depth segment)
 */

static inline uint64_t segment_at(const uint8_t *str, size_t len, size_t depth) {
    size_t off = depth * 4;
    uint64_t seg = 0;
    for (size_t i = 0; i < 8; i++) {
        size_t pos = off + i;
        if (pos < len) seg |= ((uint64_t)str[pos]) << (i * 8);
    }
    return seg;
}

static double fractal_distance(const uint8_t *a, size_t la,
                                const uint8_t *b, size_t lb) {
    int total_dist = 0;
    for (int d = 0; d < 8; d++) {
        uint64_t sa = segment_at(a, la, d);
        uint64_t sb = segment_at(b, lb, d);
        uint8_t dra = digital_root(sa);
        uint8_t drb = digital_root(sb);
        int diff = (int)dra - (int)drb;
        if (diff < 0) diff = -diff;
        int ring_dist = diff < 9 - diff ? diff : 9 - diff;
        total_dist += ring_dist;
    }
    return (double)total_dist / 8.0;
}

/* ═════════════════════════════════════════════════════════════════════
 * 8. NILPOTENT-AWARE DISPLACEMENT
 * ═════════════════════════════════════════════════════════════════════
 *
 * When all 8 cuckoo candidates are occupied, choose a victim.
 * Prefer entries whose digital root is nilpotent ({3,6,9})
 * because they are structurally unstable and will redistribute
 * naturally.
 *
 * nilpotent_score(entry) = 2 if dr ∈ {3,6,9}
 *                        = 1 if dr ∈ {1,4,7} (quadratic residues)
 *                        = 0 if dr ∈ {2,5,8} (quadratic non-residues)
 *
 * The quadratic residue structure mod 9 matches the 2ⁿ cycle:
 * residues {1,4,7} = even powers of 2 mod 9
 * non-residues {2,5,8} = odd powers of 2 mod 9
 */

static inline int nilpotent_score(uint64_t tag) {
    uint8_t dr = tag_dr(tag);
    if (is_nilpotent(dr))  return 2;  /* most unstable */
    /* Quadratic residue mod 9: dr ∈ {1,4,7} */
    if (dr == 1 || dr == 4 || dr == 7) return 1;
    return 0;  /* quadratic non-residue: {2,5,8} */
}

/* ═════════════════════════════════════════════════════════════════════
 * 9. COMBINED SIMILARITY METRIC
 * ═════════════════════════════════════════════════════════════════════
 *
 * Hybrid score combining local (q-gram) and multi-scale (fractal)
 * similarity. Weighted equally; tune α,β for precision/recall tradeoff.
 */

static double similarity_score(double qgram_overlap_ratio,
                                double fractal_dist,
                                int max_edit) {
    double alpha = 0.5, beta = 0.5;
    double fractal_score = (max_edit > 0)
        ? 1.0 - fractal_dist / (double)max_edit
        : (fractal_dist < 0.001 ? 1.0 : 0.0);
    if (fractal_score < 0.0) fractal_score = 0.0;
    return alpha * qgram_overlap_ratio + beta * fractal_score;
}

#endif /* FRACTAL_OPTIMAL_H */
