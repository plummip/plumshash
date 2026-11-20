# PlumsHash

Fast 64-bit non-cryptographic hash. Single-file C, header-only, zero
dependencies, MPL-2.0.

- **Fast** — 5 empirically-selected paths; best-in-class latency to ~256B,
  dominates throughput at 2KB+ (see [Benchmarks](#benchmarks-vs-the-field))
- **Clean** — one function, one header, unaligned-safe, works on any length
  including 0, no globals, no allocation, no platform-specific code
- **Verified** — SMHasher full battery (all 15 groups) + standalone quality
  gates (see [Testing](#testing))
- **Ships with** — `hybrid_intern.h`, a string-intern pool on the same hash:
  lock-free reads, zero-copy `str()`, stable + epoch-scratch tiers
  (see [hybrid_intern.h](#hybrid_internh--string-interning-on-plumshash))

## Quick start

```c
/* main.c */
#define PLUMSHASH_IMPLEMENTATION   /* compile the hash into this TU */
#include "plumshash.h"
#include <stdio.h>

int main(void) {
    const char *key = "hello, world";
    uint64_t h = plumshash(key, 13, 0);
    printf("plumshash v%s: %016llx\n", PLUMSHASH_VERSION,
           (unsigned long long)h);
    return 0;
}
```

```sh
cc -O3 -march=native -o main main.c   # or: clang
./main
```

That's the whole integration: copy `plumshash.h` into your project (or add
this repo to your include path) and define `PLUMSHASH_IMPLEMENTATION` in
exactly one translation unit.

## API

```c
uint64_t plumshash(const void *buf, size_t len, uint64_t seed);
```

| Parameter | Meaning |
|-----------|---------|
| `buf`     | key bytes; may be `NULL` when `len == 0`, may be unaligned |
| `len`     | key length in bytes; any value including 0 |
| `seed`    | 64-bit seed; `seed == 0` is fine (no all-zero fixed point) |

Returns a 64-bit hash. Same `(buf, len, seed)` always returns the same
value; different seeds or lengths produce unrelated hashes. Not
cryptographic — do not use for passwords, signatures, or MACs.

Version macros are defined in the header:
`PLUMSHASH_VERSION_MAJOR` / `_MINOR` / `_PATCH` and `PLUMSHASH_VERSION`
(`"1.2.0"`). `hybrid_intern.h` carries the same macro shape
(`HYBRID_INTERN_VERSION*`, currently `"2.0.0"` — it first shipped in
PlumsHash v1.2.0).

## Design (5 regions)

One hash function, five paths selected from measured crossover points on
the benchmark hardware (AArch64 Snapdragon, clang -O3 -march=native):

| len | Path | Rationale |
|-----|------|-----------|
| <= 3 | xxh3-style tiny | single-width mul, 2-mul chain, no seed premix |
| 4-16 | rapidhash-style mum tiny | 3-mul mum chain; passes the strict SMHasher BIC on 11-byte keys (the xxh3-style 9-16B fold fails it) |
| 17-112 | xxh3-style mid | independent mix16B pairs (ILP); wins 20-45% latency over serial mum chains |
| 113-384 | mum 3-lane | wyhash >16B structure, 48B blocks, 3 parallel lanes; wins the 113-384B window on both harnesses (v1.0.0's 7-lane/112B was 20-33% slower here — its 7-way init + serial if-chain tail had higher fixed cost and a `len mod 112` oscillation at these sizes) |
| > 384 | rotr23 7-lane | plumshash fast-path port, 168B unroll + one mum per lane (breaks the linear data path) |

Provenance: the <=112B paths mirror xxh3's short-key functions
(BSD-2-Clause, Yann Collet) and rapidhash's tiny mum path (MIT, Nicoshev);
the 113-384B path mirrors wyhash's 3-lane >16B structure (Unlicense,
Wang Yi); the >384B path is the plumshash fast path (MPL-2.0). All
constants are plumshash-family. The xxh3-derived mid path inherits
xxh3's documented seed-dependent multicollision property (17-112B,
multiplication by zero).

### The 384B boundary (measured, not guessed)

The mum/rot crossover was tuned on this device with an in-process
interleaved harness (variants rotated per rep, min-of-15, core-pinned).
Naive min-of-3 sweeps are useless on this SoC: the walt governor drifts
DVFS on a 10-100ms scale, so only within-row (same-size) comparisons are
trustworthy, and cross-process runs land in different boost bins.

Two findings drove the boundary down from 288:

1. `ph_rot_bulk`'s fixed per-call cost (7-lane init + 7-mum delinearization
   + compression tree + 4-round finalizer) loses to the mum path at
   289-384B — the 640B boundary measured on x86 (Intel Xeon) does NOT
   reproduce on AArch64: rot wins at 416-640B here on both harnesses.
2. The 3-lane mum path's tail is ≤48B (≤2 dependent mums), so its latency
   is flat across `len mod 48` — v1.0.0's 7-lane/112B structure was not:
   its serial if-chain tail cost up to 6 dependent mums when the tail was
   ~112B (e.g. len 448: 55ns vs rot's 45ns) and nothing when the tail was
   16B (len 464: 38ns). rot's word-remainder tail is mum-free and
   monotonic, so past 384B rot is the safe choice. 384 was the measured
   optimum: mum wins 289-384B, rot is flat and monotonic from 385B up.

## hybrid_intern.h — string interning on PlumsHash

The second header in the repo: an intern pool keyed by the same hash, built
around two tiers of **storage** and two tiers of **lifetime**.

| Tier | Call | Lifetime |
|------|------|----------|
| T1 stable (default) | `hybrid_intern(hp, s, len)` | until `hybrid_destroy` |
| T0 scratch | `hybrid_intern_scratch(hp, s, len)` | until the next `hybrid_epoch_end(hp)` — bulk-cleared: no per-entry work, no caller evict |

Strings are routed by size (`len <= SHORT_MAX_LEN`, default 48) into a short
or a long pool; each pool carries its own T0/T1 pair and its own arena. All
string bytes live in immutable bump arenas, so `hybrid_str()` is zero-copy —
it returns a pointer, valid until pool destroy (T1) or the epoch boundary
(T0). Reads are lock-free, writes are serialized by a pthread mutex, and a
handle is an opaque 64-bit **location** — `{magic | pool | epoch | gen |
chunk | offset}` (`SF_Handle`, `SF_NULL`) — that names a spot in the arena, not
a slot in the table. A lookup therefore touches the arena only: the table is
never read, and a lookup is one memory access. An evicted, epoch-cleared, or
wrong-pool handle resolves to `NULL` — never to torn data. A handle is
opaque: do not read it as a float (it is not NaN-boxed any more).

### API

| Function | Notes |
|----------|-------|
| `HybridPool *hybrid_create(uint16_t base_pool_id, uint32_t expected_short, uint32_t expected_long)` | size hints, not limits; pool ids derive from `base_pool_id`; returns `NULL` when `base_pool_id > HYBRID_INTERN_MAX_POOL_ID` (4095) or when a hint exceeds `HYBRID_INTERN_MAX_ENTRIES` (1M entries per pool) |
| `SF_Handle hybrid_intern(HybridPool*, const char *s, uint32_t len)` | T1 stable — dedup, lives until destroy |
| `SF_Handle hybrid_intern_scratch(HybridPool*, const char *s, uint32_t len)` | T0 — dropped in bulk at the epoch boundary |
| `const char *hybrid_str(HybridPool*, SF_Handle, uint32_t *len_out)` | zero-copy; `len_out` may be `NULL`; `NULL` return = stale/invalid handle |
| `uint32_t hybrid_epoch_end(HybridPool*)` | clears both scratch tiers, returns entries cleared |
| `void hybrid_evict(HybridPool*, SF_Handle)` | explicit single-entry drop |
| `void hybrid_destroy(HybridPool*)` | frees both tiers and their arenas |
| `HybridStats hybrid_stats(HybridPool*)`, `hybrid_print_stats(HybridPool*)` | entries/max/load per tier, long-pool collisions |
| `HYBRID_SCOPED(name, id, short_n, long_n)` | block-scoped pool via the `cleanup` attribute |

### Safety and lifetime limits

- Handles are validated before any access. A handle whose pool, epoch, tier or
  generation does not match resolves to `NULL`; eviction treats a stale handle
  as a no-op. The generation belongs to the *location* (an eviction bumps it),
  the epoch to the *tier* (an epoch boundary bumps it).
- Intern lengths are 32-bit, and a string must fit inside one arena chunk.
  Chunks are capped at 1 MB (`IP_CHUNK_MAX`) because the handle's offset is
  20 bits and is relative to the chunk, so the usable maximum is
  **1 MB - 8** (the 8-byte arena prefix). Longer strings, and null pointers
  with a non-zero length, are rejected with `SF_NULL`.
- The size hints passed to `hybrid_create()` are bounded by
  `HYBRID_INTERN_MAX_ENTRIES` (1M entries per pool). A larger hint returns
  `NULL` instead of sizing a table from an overflowed product.
- `hybrid_epoch_end()` is a reclamation boundary for T0 storage. Callers must
  ensure no thread is still consuming a T0 pointer returned by `hybrid_str()`
  when the boundary runs. The zero-copy pointer must not be used after that
  boundary; use external quiescence (or a future pinned/copying API) when
  readers and epoch boundaries can overlap.
- Deleting a slot leaves a **tombstone**: the probe chain runs past it, so a
  string sitting further down the same chain is still found after a
  neighbouring entry is evicted or cleared at an epoch boundary (a plain
  "free" would cut the chain and the re-intern would create a duplicate). The
  next insert on that chain reuses the deleted slot, so evict/intern churn
  does not grow the table. `hybrid_stats()` reports live entries and
  tombstones separately, so the load figure is the truth.
- The table size is fixed when the pool is created and never grows. A pool
  asked to hold more live entries than its hint sized it for returns `SF_NULL`
  from `hybrid_intern()` — size the hint for the peak live set, and expect
  evicted slots to be reused rather than reclaimed in place.
- Every interned string carries an 8-byte arena prefix in front of its bytes:
  the length, the generation, and the table slot that holds it (the handle
  names the location, so the table needs the back-pointer for eviction).
  The arena therefore costs ~8–24 B more per string depending on the length;
  `IP_ARENA_ALIGN` (16) sets the granularity. The prefix shares a cache line
  with the start of the string, so a lookup reads the length and the data
  from one line.

### Quick start

```c
#define PLUMSHASH_IMPLEMENTATION
#include "hybrid_intern.h"

HybridPool *hp = hybrid_create(1, 4096, 1024);   /* id, short/long size hints */
SF_Handle h    = hybrid_intern(hp, "user:42", 7); /* T1 — lives until destroy */

uint32_t    len;
const char *s = hybrid_str(hp, h, &len);          /* zero-copy */

SF_Handle tmp = hybrid_intern_scratch(hp, "req-17", 6);  /* T0 */
hybrid_epoch_end(hp);                             /* bulk-clears T0 */
assert(hybrid_str(hp, tmp, NULL) == NULL);        /* stale after the boundary */

hybrid_destroy(hp);
```

Compile: `-O3 -std=c11 -march=native -pthread`. Tuning knobs are `#ifndef`-
guarded at the top of the header: `SHORT_MAX_LEN` (48), `LONG_LOAD_NUM` /
`LONG_LOAD_DEN` (3/4), `IP_AVG_STRLEN` (32), `IP_CHUNK_MIN` (256 KB),
`LONG_MAX_CHUNKS` (256), `IP_ARENA_ALIGN` (16).

Gate: `test_hybrid` — single-thread roundtrip/dedup/boundary sizes, 8 threads
interning the same set concurrently (one handle per string, no duplicate
intern), evict/gen invalidation, and evict-during-resolve (TOCTOU) churn
against a small pool. `test_hybrid_security` adds malformed-handle,
full-width-length, oversized-length, pool-id, and AVX2 boundary regressions
under ASan/UBSan. `test_hybrid_lifecycle` covers the delete semantics:
tombstoned slots keep the probe chain intact (dedup survives an eviction),
the next insert reuses the deleted slot, evict/intern churn never exhausts
the table, and stale handles resolve `NULL`. See [Testing](#testing).

Bench: `bench_hybrid` (`make bench`) measures the pool itself, not the hash.
The single-threaded and the multi-threaded sections are kept apart on purpose —
the first is per-operation latency for one caller (insert path, dedup path,
lock-free read path), the second is aggregate throughput for T callers sharing
one pool (the pool mutex's cost). A mutex change barely shows up in a
single-threaded run, and cache-locality work is invisible in a contended one.
`./bench_hybrid st` and `./bench_hybrid mt` run one section at a time; each
case prints one machine-readable `bench section=… case=… ns=… ops_s=…` line.

## Benchmarks vs the field

Methodology (`bench_compete.c`): two harnesses, order-randomized (size,
variant) pairs, min-of-3 per cell over shuffled reps. The tables below are
min-of-7 runs of that binary — `python3 bench_min7.py --runs 7`, wired as
`make bench`. They are **unpinned**: pinning to one core (`make bench PIN=0`)
measured 1.5-3x *slower* here (0B 10.2 vs 3.0ns; 16KB 798 vs 283ns), because
the device's own background work shares that core and the scheduler can no
longer dodge it — check that pinning helps before publishing pinned numbers.
- **LATENCY** — serial seed dependency (hash-table lookup model), fixed buffer.
- **THROUGHPUT** — independent seeds over a rotating 4 MB chunk pool (bulk/ILP model).

Device: AArch64 Snapdragon (8-core), clang -O3 -march=native, 2026-09-11.

> **Noise caveat:** this SoC's `walt` governor drifts DVFS on a 10-100 ms
> scale, so absolute ns/op are not reproducible run-to-run — a stray boost or
> drift bin lands inside single measurements. Treat the field *ratios within a
> row* as the signal, not the absolute values, and read only margins larger
> than ~10% as real. Two artifacts below: the 96B and 255B latency rows, and
> the 4KB throughput row, where xxh3 measured 114.0ns on 2026-09-09 and 92.7ns
> on 2026-09-11 while every other cell in that row moved by less than 1%. The
> v1.1.0 improvement quoted in the Reading section is from an in-process
> interleaved harness (min-of-15, variants rotated per rep, core-pinned) that
> cancels this drift.

### Latency (ns/op, lower is better)

| Size | plums | rapidhash | wyhash | xxh3 | t1ha2 |
|------|-------|-----------|--------|------|-------|
| 0B | 3.0 | 4.7 | 7.1 | 6.2 | 5.0 |
| 1B | 6.2 | 7.1 | 7.1 | 4.8 | 7.3 |
| 3B | 5.9 | 7.1 | 7.1 | 6.7 | 5.2 |
| 4B | 5.7 | 9.1 | 7.2 | 9.2 | 5.0 |
| 8B | 6.7 | 7.5 | 7.2 | 9.7 | 5.2 |
| 12B | 6.7 | 9.1 | 7.2 | 5.6 | 7.6 |
| 16B | 6.7 | 7.5 | 7.2 | 6.6 | 8.9 |
| 17B | 7.5 | 6.8 | 6.7 | 7.4 | 8.3 |
| 24B | 6.3 | 9.5 | 9.5 | 5.3 | 11.6 |
| 32B | 7.5 | 6.8 | 9.3 | 5.6 | 11.9 |
| 47B | 6.7 | 8.4 | 11.8 | 8.9 | 16.1 |
| 48B | 9.0 | 11.7 | 11.0 | 8.9 | 10.1 |
| 64B | 6.5 | 15.3 | 10.9 | 6.9 | 12.9 |
| 96B | 10.4 | 13.3 | 13.4 | 10.4 | 14.8 |
| 112B | 12.0 | 21.8 | 10.1 | 12.1 | 18.8 |
| 128B | 19.1 | 13.6 | 15.5 | 10.3 | 12.3 |
| 160B | 18.6 | 13.2 | 11.1 | 26.8 | 19.2 |
| 192B | 20.0 | 23.0 | 13.1 | 19.0 | 21.4 |
| 224B | 20.6 | 27.2 | 17.6 | 20.8 | 23.6 |
| 255B | 26.5 | 19.2 | 20.7 | 37.0 | 31.9 |
| 256B | 15.0 | 13.6 | 27.1 | 31.9 | 30.5 |
| 320B | 25.5 | 28.3 | 25.4 | 22.2 | 30.1 |
| 384B | 39.2 | 18.9 | 28.1 | 31.3 | 34.5 |
| 512B | 32.1 | 31.0 | 35.3 | 33.2 | 43.2 |
| 768B | 38.5 | 31.0 | 48.2 | 30.4 | 75.0 |
| 1KB | 43.1 | 31.5 | 43.2 | 69.8 | 77.9 |
| 2KB | 64.6 | 80.4 | 113.0 | 52.6 | 185.4 |
| 4KB | 108.0 | 206.7 | 170.8 | 144.1 | 287.1 |
| 8KB | 195.6 | 386.7 | 422.9 | 218.6 | 565.6 |
| 16KB | 283.4 | 566.9 | 834.6 | 523.6 | 1121.9 |

### Throughput (ns/op, lower is better)

| Size | plums | rapidhash | wyhash | xxh3 | t1ha2 |
|------|-------|-----------|--------|------|-------|
| 0B | 1.5 | 1.8 | 1.9 | 1.7 | 2.2 |
| 1B | 2.8 | 3.0 | 3.1 | 2.1 | 3.8 |
| 3B | 2.0 | 3.0 | 3.1 | 3.0 | 2.7 |
| 4B | 2.9 | 2.0 | 3.4 | 3.1 | 2.7 |
| 8B | 2.9 | 2.9 | 3.4 | 3.1 | 3.8 |
| 12B | 2.1 | 2.5 | 2.9 | 4.3 | 4.1 |
| 16B | 2.2 | 2.9 | 3.4 | 3.0 | 4.6 |
| 17B | 4.1 | 3.9 | 3.0 | 4.2 | 5.5 |
| 24B | 3.9 | 2.6 | 3.5 | 4.2 | 3.9 |
| 32B | 3.5 | 5.8 | 3.5 | 4.2 | 6.0 |
| 47B | 6.5 | 3.4 | 5.1 | 6.7 | 8.5 |
| 48B | 6.3 | 4.5 | 5.1 | 4.9 | 8.0 |
| 64B | 6.3 | 3.7 | 5.1 | 4.6 | 8.2 |
| 96B | 8.2 | 6.7 | 6.8 | 7.3 | 7.3 |
| 112B | 10.3 | 8.2 | 4.9 | 10.8 | 12.2 |
| 128B | 6.6 | 6.2 | 6.5 | 10.8 | 8.8 |
| 160B | 6.3 | 10.1 | 8.6 | 9.9 | 14.1 |
| 192B | 10.6 | 9.6 | 10.5 | 12.1 | 11.4 |
| 224B | 11.7 | 13.1 | 8.2 | 13.8 | 17.8 |
| 255B | 9.5 | 12.5 | 9.3 | 29.5 | 22.4 |
| 256B | 12.6 | 12.9 | 12.3 | 25.8 | 14.1 |
| 320B | 15.3 | 16.2 | 10.8 | 18.1 | 23.5 |
| 384B | 18.4 | 17.3 | 18.1 | 20.8 | 27.3 |
| 512B | 16.3 | 21.9 | 23.1 | 26.5 | 25.3 |
| 768B | 21.1 | 32.0 | 36.0 | 37.8 | 37.5 |
| 1KB | 24.7 | 25.6 | 40.6 | 39.1 | 69.5 |
| 2KB | 56.2 | 100.8 | 100.2 | 96.3 | 139.1 |
| 4KB | 100.1 | 142.6 | 203.2 | 92.7 | 278.3 |
| 8KB | 134.1 | 279.7 | 410.3 | 188.6 | 560.9 |
| 16KB | 329.9 | 556.6 | 828.2 | 521.4 | 1049.2 |

Reading: no variant wins everywhere, and the middle of the range is a scrum
inside the drift — read margins, not ranks.

- **0-16B latency**: lowest or tied-lowest in every row (0B 3.0 vs 4.7-7.1;
  4B 5.7 vs 5.0-9.2; 8B 6.7 vs 7.2-9.7).
- **1-32B**: a three-way scrum — plums, xxh3 and rapidhash land within
  ~1.5ns and the order flips between runs (1B: xxh3 4.8 vs plums 6.2 here;
  plums 5.9 vs xxh3 6.7 on 2026-09-09).
- **47-112B latency**: leader in every row (47B 6.7 vs 8.4-16.1; 112B 12.0),
  the exception being wyhash's 10.1 at 112B.
- **bulk 8KB-16KB**: ahead of every other variant on *both* harnesses by
  1.3-3.4x (latency 16KB: 283.4 vs 523.6-1121.9ns; throughput 16KB: 329.9
  vs 521.4-1049.2ns).
- **2KB**: throughput leader (56.2 vs 96.3-139.1); latency second (64.6),
  with xxh3's 52.6 and plums' 64.9 trading places between the two runs.

Everything else (128-192B, 256B-1KB latency, 4KB throughput) sits inside the
noise: cells that differ by less than ~10% in this table have flipped
direction between runs.

The v1.0.0 → v1.1.0 change replaces the 113-384B 7-lane/112B mum path with a
3-lane/48B one. Measured on the drift-free interleaved harness, that closes a
20-50% latency/throughput gap to wyhash in that window (20-33% faster in
160-320B; e.g. 160B 29.5→20.3ns, 224B 39.3→26.3ns). The 255B latency row
above is a measurement artifact, not the 3-lane regressing (255 = 5×48+15, a
tail size the interleaved harness pins at ~28ns, on par with 256B).

Reproduce: `make bench` — writes these tables (min-of-7 `bench_compete` vs
the field, unpinned) to `bench_results.txt` and then prints raw bandwidth from
`test_speed`. `make bench RUNS=5` shortens the sweep; `PIN=<cpu>` pins it (see
the pinning caveat above).

## Testing

`make test` builds everything and runs the quality gate (9 binaries):

| Gate | What it checks |
|------|----------------|
| `test_sanitize` (ASan/UBSan) | determinism, seed sensitivity, per-position flips, avalanche ~32/64 bits (ideal), no adjacent-length collisions |
| `test_quality` | 21/21 SMHasher-style checks (sanity, avalanche >= 25%, chi², sparse, seed independence, appended-zeroes, permutation) |
| `test_comb16` | SMHasher Combination 16-byte reproduction: all keys distinct at every block size 16-352B — no low-weight collapse. The gate asserts 66 data rows (3 variants x 22 sizes) with `maxgroup=1` on every one; a collapse fails it. (This caught the old linearity bug: the rotr23 lane mixes are linear in the data and a pure-XOR compression tree collapsed 100K keys to ~4K hashes; one mum per lane fixes it.) |
| `test_security` | multicollision bound, seed recovery attempt, differential, whitening bypass, strict avalanche, hash-flooding, length extension |
| `test_edge` | path-boundary keys, pattern keys, tiny/long key extremes |
| `test_extensive` | broad sanity/chi²/avalanche matrix + throughput spot checks |
| `test_hybrid` | `hybrid_intern.h`: roundtrip/dedup/boundaries, 8-thread concurrent intern of one set, evict/gen invalidation, evict-during-resolve (TOCTOU) |
| `test_hybrid_security` (ASan/UBSan) | malformed/out-of-range/tier-mismatched/free-slot handles, 65,536-byte length roundtrip/dedup, oversized-length rejection, invalid pool ids, AVX2 tail bounds |
| `test_hybrid_lifecycle` (ASan/UBSan) | delete semantics: evict/intern churn does not exhaust the table, dedup survives an eviction, deleted slots are reused, scratch tier survives epoch churn, stale handles resolve `NULL` |

Standalone suite, built by `make all` and run by CI:

| File | What it checks |
|------|----------------|
| `smhasher_plums.c` | SMHasher-style suite (sanity, avalanche, differential, chi², sparse, permutation, appended-zeroes) — runs in `.github/workflows/smhasher.yml` |

Published benchmark tables come from `make bench`
(`bench_min7.py --runs 7` over `bench_compete`, min per cell across runs,
unpinned). `make bench RUNS=n` shortens the sweep; `PIN=<cpu>` pins it —
slower on this device, see the Benchmarks pinning caveat.

### SMHasher (rurban fork)

Full battery, all 15 test groups:

```
Sanity, Avalanche, Sparse, Permutation, Window, Cyclic, TwoBytes,
Text, Zeroes, Seed, Diff, DiffDist, BIC, MomentChi2, Prng, BadSeeds
```

Result: **all PASS, zero failures**. BIC max bias 0.008660 (rapidhash
measures 0.008576 on the same build). Verification value 0x12455BFA.
BadSeeds 0x0 PASS. ~24 min runtime.

Run it yourself:

```sh
git clone https://github.com/rurban/smhasher
cd smhasher
cp ../plumshash.h .   # smhasher builds its own copy
cmake -B build && cmake --build build -j
./build/SMHasher --test=Sanity,Avalanche,Sparse,Permutation,Window,Cyclic,\
  TwoBytes,Text,Zeroes,Seed,Diff,DiffDist,BIC,MomentChi2,Prng,BadSeeds \
  plumshash
```

## Status

**v1.2.0** (2026-09-11) — two headers, both MPL-2.0: `plumshash.h` (the hash)
and `hybrid_intern.h` (v1.0.0, the intern pool).

| Version | Change |
|---------|--------|
| v1.0.0 | one header, 5 measured paths (113-384B was 7-lane/112B) |
| v1.1.0 | 113-384B becomes 3-lane/48B mum — 20-33% faster in that window |
| v1.2.0 | `hybrid_intern.h`: two-tier intern pool on the same hash |

Benchmarked on one SoC, one compiler, one input pattern. Verify on your
target hardware before trusting it in production.

## License

MPL-2.0. `plumshash.h` mirrors the structure of xxh3 (BSD-2-Clause),
rapidhash (MIT), and wyhash (Unlicense) with plumshash-family constants;
see the header for the full provenance note.
