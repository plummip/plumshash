# PlumsHash intern pool — fast table design

**Date:** 2026-11-20
**Status:** draft for review
**Scope:** `hybrid_intern.h` (the intern pool), its gate tests, and a new benchmark.
**Trigger:** request for "the fastest theoretically possible" intern table, targeting
both single-threaded lookup latency and multi-threaded intern throughput.

---

## 1. Goals

1. **Lookup latency.** `hybrid_str()` should cost one DRAM round trip, not two or more.
2. **Intern throughput.** The per-pool `pthread_mutex` currently serializes every
   intern; the target is a path that scales across cores.
3. **Index stability.** A string's slot index must be a pure function of
   `(bytes, length)` — no dependence on insertion order, probe history, or
   wall-clock entropy — so the table layout is reproducible and cache behaviour
   is predictable.
4. **Correctness first.** Two live bugs in the shipped code (§2.1) are fixed
   before any performance work lands.

**Non-goals**

- No change to `plumshash.h` — the hash function itself is not up for redesign.
- No change to the public API signatures (`hybrid_create`, `hybrid_intern`,
  `hybrid_intern_scratch`, `hybrid_str`, `hybrid_epoch_end`, `hybrid_evict`,
  `hybrid_destroy`, `hybrid_stats`, `hybrid_print_stats`, `HYBRID_SCOPED`).
  The *encoding* of `SF_Handle` is internal and may change (it is opaque to
  callers and never persisted).
- No multi-process / shared-memory pool.

---

## 2. Findings in the current implementation

All line references are `hybrid_intern.h` at commit `59a09c8`.

### 2.1 P0 — eviction and epoch clear free the wrong byte

`Entry` is two 64-bit words; word 0 packs `hash_gen` (bytes 0–3), `kind`
(byte 4), `chunk_id` (byte 5), `tier` (byte 6). On a little-endian load,
`kind` is therefore bits 32–39 and `tier` is bits 48–55.

Both free paths clear the **tier** byte instead of the **kind** byte:

```c
nhdr &= ~(0xFFull << 48);   /* comment says "kind = LK_FREE" — it clears tier */
```

(`pool_evict`, `pool_epoch_end`).

Consequences, reproduced on-device:

| symptom | evidence |
|---|---|
| the slot is never freed (`kind` stays `LK_OWNED`) | raw header before `000100015bbf0201` → after `000000015bbf0202`: kind byte `1 → 1`, tier byte `1 → 0` |
| the table fills up and interning starts failing | evict+intern loop: intern returned `SF_NULL` after exactly 128 iterations (M = 128) |
| the stats under-report | `hybrid_stats` reported `short_entries=0` while the table was 100 % full |
| the scratch tier dies after M scratch interns | `pool_epoch_end` has the same mask; T0 slots are never reclaimed across epochs |

Fixing only the mask is **not** sufficient: with the kind byte genuinely
cleared, the probe stops at the hole and a re-intern of a string that lives
later in the same probe chain would insert a **duplicate** instead of finding
the existing entry. Deletion therefore needs real tombstone semantics
(§4.1), not a one-byte patch.

### 2.2 The index derivation is already optimal

```c
uint64_t h  = plumshash(s, len, p->seed);
uint32_t h32 = (uint32_t)h;          /* 64-bit hash truncated to 32 */
uint32_t pos = h32 & (p->M - 1);     /* M is a power of two, fixed at create */
```

`M` is set once in `pool_create` and there is **no rehash path anywhere in the
file**, so a string's slot index is already stable for the pool's lifetime and
the extraction is already a single AND — the fastest form possible.

Consequences for the original idea (length-derived shift, odd/even shift
direction): PlumsHash's output is uniformly mixed in all 64 bits
(`ph_final_plums`, four-round finalizer, length folded into the final
multiply), so *any* window of it is uniform. A length-derived shift amount
costs one extra instruction on the address-dependency chain and changes the
distribution not at all. Length parity is worse than free: in ASCII text,
length parity **is** last-byte parity, so it is correlated with content rather
than an independent bit.

The length-derived shift is therefore retained only as a compile-time
experiment switch, default **off**, and is documented as distribution-neutral.

Two real defects do exist in the same area:

- the index uses only the **low 32 bits** of a 64-bit hash;
- the stored fingerprint (`hash_gen` bits 31–8) is taken from hash bits 8–31,
  which **overlap** the index bits (0–20 at `M = 2^21`) by 13 bits — so the
  cheap pre-`memcmp` reject cannot discriminate candidates that share a bucket.

### 2.3 Where the time actually goes

`Entry` is 32 bytes, so the table holds **2 slots per cache line** and every
step of the linear probe is a separate DRAM miss. At the maximum table size
(`M = 2^21`) the header array is **64 MB**. A hit costs: header miss + arena
miss = 2 round trips; a probe chain costs *N* round trips.

| cost | today | target |
|---|---|---|
| probe | N DRAM misses | ctrl-array scan, mostly L2 hits |
| hit | 2 DRAM misses (entry + data) | 1 DRAM miss (the data) |
| insert | one mutex per intern | no global serialization |

### 2.4 There is no intern benchmark

`bench_compete.c` and `test_speed.c` benchmark the *hash*. Nothing measures
the intern pool. "Fastest" is currently unmeasurable, and the cache cliff
(table larger than L2) has never been observed.

---

## 3. Design overview

Four phases, each independently shippable and gated.

| phase | what | risk |
|---|---|---|
| **0** | correct delete semantics (tombstones) + regression tests | low |
| **1** | `bench_hybrid.c` — the measurement harness | none |
| **2** | table redesign: ctrl array + SIMD group probe + 8-byte slots | medium |
| **3** | location-carrying handle → 1-miss lookups, table rehashable | medium |
| **4** | lock-free insert path (CAS), replacing the per-pool mutex | high |

Phase 0 is a prerequisite for the rest: phases 2–4 all rewrite the probe, and
the tombstone model has to be right before it is moved.

---

## 4. Design

### 4.1 Phase 0 — correct delete semantics (P0)

Model the slot state machine explicitly instead of overloading one byte. In this
phase the state lives in the existing `kind` byte of `Entry` (bits 32–39); the
dedicated ctrl array with an embedded tag arrives in phase 2.

```
kind byte:  LK_FREE  (0)    EMPTY    — probe stops here
            LK_TOMB  (2)    DELETED  — tombstone, probe continues past it
            LK_OWNED (1)    OCCUPIED
```

- Probe continues past `LK_TOMB` slots; it stops only at `LK_FREE`.
- Insert reuses the **first** `LK_TOMB` slot found on the chain (so tombstones
  do not accumulate faster than necessary); otherwise the first `LK_FREE`.
- **No rehash in this phase.** A rehash re-inserts live entries at new
  positions, and the handle encodes the slot index — so rehashing would
  invalidate every live handle. Reclaiming the table therefore waits for
  phase 3, where the handle names an arena location instead of a slot and the
  table becomes free to rebuild at any time. Until then the table size is
  fixed at `pool_create`, tombstones are reused by later inserts, and a pool
  that runs out of slots reports `SF_NULL` from `hybrid_intern()` rather than
  silently misbehaving.
- `pool_evict` clears the **kind** byte (bits 32–39) and marks the slot
  `LK_TOMB`; `pool_epoch_end` does the same for every T0 slot it clears.
- `n_entries` counts live entries; a separate counter tracks tombstones so
  `hybrid_stats` can report load honestly.

Regression tests (new file `test_hybrid_lifecycle.c`, added to the gate):

1. evict + intern loop of `4 × M` iterations → no intern returns `SF_NULL`,
   and `hybrid_stats` reports the true live count.
2. dedup survives eviction: intern A and B that share a probe chain, evict A,
   re-intern B → the **same** handle comes back, no duplicate entry.
3. scratch churn: `epoch_end` × `M` cycles with scratch interns in between →
   the scratch tier keeps working and live counts stay correct.
4. stale handles after eviction and after `epoch_end` resolve to `NULL`.

### 4.2 Phase 1 — measurement harness

New `bench_hybrid.c`, wired into `make bench` (not `make test`). It reports:

- **intern miss-heavy**: N unique strings, ns/intern, entries/s.
- **intern hit-heavy**: re-intern the same N strings, ns/intern.
- **lookup**: `hybrid_str` over random live handles, ns/lookup.
- **long string**: 1 KB and 64 KB strings (hash + copy dominated).
- **multi-threaded**: T threads interning disjoint string sets, aggregate
  throughput, plus the same test with one shared pool (the mutex's cost).
- **table size sweep**: 10^4 → 10^6 entries, so the cache cliff is visible.

Each case prints a stable machine-readable line so before/after numbers can be
diffed in a commit message. The baseline is captured on the current code
before phase 2 starts.

### 4.3 Phase 2 — table redesign

Per pool:

```
ctrl[M]      1 byte per slot   tag(7) | state(1)      -> 2 MB at M = 2^21
slots[M]     8 bytes per slot  chunk(10) | offset(20) | len(32) | spare(2)
```

- **Index** from the **full 64-bit** hash: `idx = (uint32_t)h & (M - 1)`.
- **Tag** from the **top** 7 bits of the hash (`h >> 57`), so tag and index
  are disjoint — the fast reject finally discriminates same-bucket candidates.
- **Probe** in groups of 16 (SSE2) or 32 (AVX2): one vector load of the ctrl
  bytes, `cmpeq` against the broadcast tag, `movemask` → a candidate bitmask;
  only candidate slots are touched. A group of 16 ctrl bytes is 16 bytes of one
  cache line — the scan stays in cache while the *entries* live in DRAM.
- **Slot array** replaces the 32-byte `Entry` header: 8 B/slot instead of
  32 B/slot, so the table shrinks **4×** (64 MB → 16 MB at max size) and the
  ctrl array is 32× denser than the old header array.
- **Delete** semantics as in phase 0; the tag is preserved in the tombstone so
  a probe can still skip it correctly.
- The `Entry` struct disappears; `hash_gen`/`gen`/`chunk_id`/`tier`/`len`/
  `offset` are replaced by the slot + the arena prefix (§4.4).

Fallback: with no SSE2/AVX2, a scalar loop over the same ctrl bytes (still
cache-dense) keeps the behaviour identical.

### 4.4 Phase 3 — location-carrying handle

`SF_Handle` stops naming a table slot and names a **location in the arena**:

```
bit 63      magic (1)         1 for a valid handle; SF_NULL == 0
bits 62..50 pool id (13)
bits 49..38 epoch (12)        per tier; bumped by epoch_end (T0 only)
bits 37..30 gen (8)           per-location generation
bits 29..20 chunk id (10)     chunk ids 0..511 = T0, 512..1023 = T1
bits 19..0  byte offset (20)  within the chunk
```

The tier is **not** a separate field — it is derived from the chunk id
(chunks 0–511 belong to the scratch arena, 512–1023 to the stable arena), and
the epoch is per tier so that `epoch_end` can invalidate scratch handles
without touching stable ones.

- The **length and generation live in the arena**, in an 8-byte prefix stored
  immediately before the string bytes:
  `[gen(8) | len(32)]` then the bytes. The prefix shares a cache line with the
  start of the string, so `hybrid_str()` reads length and data from one miss.
- `hybrid_str(handle)`:
  1. validate magic / pool, derive the tier from the chunk id, and compare the
     tier's epoch → `NULL` if stale;
  2. `chunk.base + offset` → read the prefix → compare `gen` → `NULL` if the
     location was evicted or reused;
  3. return the pointer.

  **One DRAM round trip, and the table is never touched on the read path.**
- Staleness is detected by two independent fields: `epoch` (pool-wide, catches
  scratch entries invalidated by `epoch_end`) and `gen` (per-location, catches
  eviction and slot reuse).
- The NaN box is dropped: the magic bit plus the tier field are enough to
  discriminate a valid handle from `SF_NULL`.

**Costs and consequences of this choice**

- The arena grows by 8 bytes per string, plus the rounding delta: the
  allocation unit becomes `round_up(8 + len, 32)` instead of `round_up(len, 32)`
  — an average overhead of ~25 B/string, ~78 % on top of `IP_AVG_STRLEN = 32`.
  *Mitigation under consideration:* drop the alignment granularity from 32 to
  16 bytes (the stored side no longer needs 32-byte slack now that the AVX2
  tail uses `memcmp`), which cuts the overhead to ~12 B/string.
- Chunk size is capped at 1 MB (20-bit offset). Chunk ids 0–511 serve the
  scratch arena and 512–1023 the stable one — 512 MB per tier, 1 GB per pool.
- **A string longer than 1 MB can no longer be interned.** Today the cap is
  `UINT32_MAX - 31`. Open decision (§7.1): accept the 1 MB cap, or add an
  oversize path.
- `HYBRID_INTERN_MAX_POOL_ID` stays 4095 (13 bits ✓).

### 4.5 Phase 4 — insert path

The mutex is the throughput ceiling: every intern takes the pool lock.

Target: lock-free insert.

1. Probe the ctrl array (lock-free reads).
2. On a tag match, compare the slot's length and `memcmp` the data → hit →
   return the existing handle.
3. On no match, allocate arena space with the existing atomic bump allocator,
   copy the bytes, publish the prefix.
4. Claim a slot with a CAS on the ctrl byte (`EMPTY`/`DELETED` → `RESERVED` →
   `OCCUPIED`); the tag is written with the tag bits and the state with the
   high bit.
5. If another thread claimed the same slot first, re-check for the duplicate
   and return the winner's handle. The loser's arena copy is wasted space
   (acceptable; a free-list can reclaim it later).

Correctness under concurrent insert of the *same* string is the delicate part:
the CAS on the ctrl byte plus the post-CAS duplicate check must guarantee that
exactly one entry exists per string. This is the phase's main risk and gets its
own stress test (N threads × the same M strings, assert one entry each).

---

## 5. Data formats

| structure | layout | size |
|---|---|---|
| ctrl | `state(1) | tag(7)` | 1 B/slot |
| slot | `chunk(10) | offset(20) | len(32) | spare(2)` | 8 B/slot |
| arena prefix | `gen(8) | len(32)` | 8 B/string |
| handle | magic(1) | pool(13) | epoch(12) | gen(8) | chunk(10) | offset(20) | 64 bits |

---

## 6. Testing and acceptance

**Correctness** (all part of `make test`)

- The existing 9 gate binaries stay green, including the ASan/UBSan gate.
- New `test_hybrid_lifecycle.c` (§4.1) — 4 scenarios, run under ASan/UBSan.
- New `test_hybrid_table.c` — ctrl/slot/tag layout, group-probe equivalence
  against a scalar reference probe, tombstone reuse, and index stability (the
  same string always lands on the same index for a fixed table size and
  seed). The table-rehash case moves to phase 3, where the handle survives a
  rebuild.
- The existing `test_hybrid_security.c` keeps covering handle forgery.

**Performance** (in `make bench`, reported in the commit message)

- lookup: ≥ 2× lower ns/lookup at 10^6 entries (the miss-dominated regime).
- intern (miss-heavy, single thread): ≥ 1.5× lower ns/intern.
- intern (multi-threaded, one pool): ≥ 3× aggregate throughput at 4 threads
  versus the mutex baseline.
- memory: table bytes per slot must fall from 32 B to ≤ 9 B; the arena
  overhead from the prefix must stay ≤ 32 B/string.

**Behaviour**

- `hybrid_stats` must report live entries, tombstone count and load honestly.
- The public API signatures and semantics are unchanged.

---

## 7. Open decisions

1. **Max interned length.** Phase 3 caps it at 1 MB. Accept the cap, or keep
   4 GB with an oversize path (extra complexity in the handle and the arena)?
   *Recommendation:* accept 1 MB — a string intern pool holding 4 GB strings is
   not a real use case, and the cap keeps the handle at 64 bits.
2. **Seed.** The pool seed is random (`_entropy()`), so indices are not
   reproducible across runs even though the derivation is deterministic. Add an
   optional caller-supplied seed (`hybrid_create_ex`) for reproducible layouts?
   *Recommendation:* yes, additive, default remains random.
3. **Arena alignment granularity.** 32 bytes (today, +25 B/string average) or
   16 bytes (+12 B)? *Recommendation:* 16, once phase 2 has removed the
   32-byte-slack requirement.
4. **Pool id width.** The handle keeps 13 bits, so `HYBRID_INTERN_MAX_POOL_ID`
   stays 4095. Fine as is.

---

## 8. Migration

- `HYBRID_INTERN_VERSION` → `2.0.0` when the handle format changes (phase 3).
- The header-only, source-compatible contract is preserved: callers recompile,
  nothing on disk is persisted, no API call changes.
- Phase 0, 1 and 2 can ship independently; each lands with its own tests and a
  benchmark delta in the commit message.

---

## 9. Risks

| risk | mitigation |
|---|---|
| tombstone logic breaks dedup | phase 0 lands first, with its own stress test; phase 2 keeps a scalar reference probe to differential-test against |
| +25 B/string arena overhead | alignment granularity reduction (§7.3); make it configurable and measure |
| lock-free insert creates duplicate entries | the phase-4 stress test asserts exactly one entry per string under contention; phase 4 is the only phase that may be descoped |
| SIMD path diverges from the scalar path | differential test: same input, both probes, identical handles |
| 1 MB length cap breaks a caller | documented in the README; the cap is a compile-time constant that can be raised if the handle gains a bit |
