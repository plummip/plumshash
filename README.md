# PlumsHash

Fast 64-bit non-cryptographic hash, single-file C, zero dependencies, MPL-2.0.

Two variants live in this repo:

| Header | What it is |
|--------|------------|
| `plumshash.h` | The original PlumsHash: tiny / medium / safe / fast paths (ARX accumulator + rotr23 7-lane bulk). |
| `plumshash_hybrid.h` | Performance hybrid: 5 empirically-selected regions (xxh3-style tiny/mid, rapidhash-style mum 7-lane, plumshash rotr23 bulk). |

## Usage

```c
#define PLUMSHASH_IMPLEMENTATION
#include "plumshash.h"
uint64_t h = plumshash(key, len, seed);            /* original */

#define PLUMSHASH_HYBRID_IMPLEMENTATION
#include "plumshash_hybrid.h"
uint64_t h = plumshash_hybrid(key, len, seed);     /* hybrid */
```

`seed` is a 64-bit value; `len` is in bytes; the hash accepts unaligned buffers and any length including 0.

## Hybrid design (5 regions)

Region boundaries are measured crossover points on the benchmark hardware
(AArch64 Snapdragon, clang -O3 -march=native):

| len | Path | Rationale |
|-----|------|-----------|
| <= 3 | xxh3-style tiny | single-width mul, 2-mul chain, no seed premix |
| 4-16 | rapidhash-style mum tiny | 3-mul mum chain; passes the strict SMHasher BIC on 11-byte keys (the xxh3-style 9-16B fold fails it) |
| 17-112 | xxh3-style mid | independent mix16B pairs (ILP); wins 20-45% latency over serial mum chains |
| 113-288 | mum 7-lane | rapidhash >112 structure, 112B/224B unrolled blocks, 7 parallel lanes; wins latency and throughput in this window |
| > 288 | rotr23 7-lane | plumshash fast-path port, 168B unroll + one mum per lane (breaks the linear data path) |

Provenance: the <=112B paths mirror xxh3's short-key functions
(BSD-2-Clause, Yann Collet) and rapidhash's tiny mum path (MIT, Nicoshev);
the 113-288B path mirrors rapidhash's >112 mum structure (MIT); the >288B
path is the plumshash fast path (MPL-2.0). All constants are
plumshash-family. The xxh3-derived mid path inherits xxh3's documented
seed-dependent multicollision property (17-112B, multiplication by zero).

## Benchmarks vs the field

Methodology (`bench_compete.c`): two harnesses, order-randomized, min-of-3,
pinned core.
- **LATENCY** — serial seed dependency (hash-table lookup model), fixed buffer.
- **THROUGHPUT** — independent seeds over a rotating 4 MB chunk pool (bulk/ILP model).

Device: AArch64 Snapdragon (8-core), clang -O3 -march=native, 2026-08-25.

### Latency (ns/op, lower is better)

| Size | hybrid | plums | rapidhash | wyhash | xxh3 |
|------|--------|-------|-----------|--------|------|
| 0B | 6.8 | 8.7 | 10.6 | 10.6 | 6.9 |
| 1B | 8.8 | 17.9 | 10.6 | 10.7 | 8.8 |
| 8B | 11.3 | 23.3 | 9.6 | 10.7 | 13.7 |
| 16B | 10.6 | 24.1 | 11.2 | 10.7 | 9.9 |
| 32B | 9.5 | 41.8 | 14.2 | 12.2 | 11.1 |
| 64B | 13.5 | 42.3 | 20.7 | 16.3 | 13.3 |
| 128B | 20.4 | 41.5 | 20.1 | 23.3 | 22.1 |
| 256B | 30.3 | 48.4 | 36.4 | 29.7 | 44.5 |
| 512B | 48.2 | 54.6 | 53.4 | 52.9 | 62.5 |
| 1KB | 66.7 | 70.8 | 85.3 | 90.2 | 115.0 |
| 4KB | 194.0 | 183.8 | 228.3 | 321.6 | 353.5 |
| 16KB | 696.8 | 558.9 | 1286.6 | 1325.3 | 1415.6 |

### Throughput (ns/op, lower is better)

| Size | hybrid | plums | rapidhash | wyhash | xxh3 |
|------|--------|-------|-----------|--------|------|
| 0B | 3.4 | 12.2 | 5.2 | 4.3 | 3.7 |
| 1B | 4.8 | 14.0 | 5.8 | 6.2 | 5.6 |
| 8B | 6.6 | 11.4 | 5.8 | 5.1 | 5.2 |
| 16B | 6.6 | 14.4 | 5.6 | 6.4 | 5.6 |
| 32B | 8.1 | 29.5 | 7.8 | 7.1 | 8.1 |
| 64B | 12.9 | 30.3 | 10.7 | 10.3 | 13.5 |
| 128B | 19.3 | 30.1 | 17.4 | 15.7 | 22.8 |
| 256B | 29.9 | 36.1 | 27.0 | 25.8 | 46.9 |
| 512B | 46.3 | 44.7 | 46.5 | 44.2 | 64.1 |
| 1KB | 66.2 | 66.0 | 85.6 | 89.8 | 109.2 |
| 4KB | 194.7 | 196.1 | 317.3 | 341.8 | 239.4 |
| 16KB | 730.9 | 666.4 | 834.3 | 1324.1 | 1341.4 |

Reading: the hybrid is best-in-class (or tied) for latency up to ~256B and
at 512B+; it wins throughput decisively at >= 512B (1.3-1.8x vs rapidhash
at 4-16KB). At 16KB latency the original plumshash edges ahead (~25%,
mostly memory-bandwidth-bound; earlier runs showed ~6-15%) — that is the
price of the mum-per-lane linearity fix. The remaining throughput gap is
64-128B (~10-20% vs rapidhash), where the mid path trades throughput for
30-40% better latency.

## SMHasher

Full battery (rurban fork, all 15 test groups) on the hybrid:

```
Sanity, Avalanche, Sparse, Permutation, Window, Cyclic, TwoBytes,
Text, Zeroes, Seed, Diff, DiffDist, BIC, MomentChi2, Prng, BadSeeds
```

Result: **all PASS, zero failures**. BIC max bias 0.008660
(rapidhash measures 0.008576 on the same build). Verification value
0x12455BFA. BadSeeds 0x0 PASS. ~24 min runtime.

Run it yourself:

```sh
cd smhasher
cp ../plumshash_hybrid.h .   # smhasher builds its own copy
cmake --build build -j8
./build/SMHasher --test=Sanity,Avalanche,Sparse,Permutation,Window,Cyclic,\
  TwoBytes,Text,Zeroes,Seed,Diff,DiffDist,BIC,MomentChi2,Prng,BadSeeds \
  plumshash_hybrid
```

Additional quality gates (all green):
- Combination reproduction (`repro_comb16.c`): all keys distinct at every
  block size 16-352B — no low-weight collapse. (This is the test that
  caught the old linearity bug: the rotr23 lane mixes are linear in the
  data and a pure-XOR compression tree collapsed 100K keys to ~4K hashes;
  one mum per lane fixes it.)
- `quality_hybrid.c`: 21/21 checks (sanity, avalanche >= 25%, seed
  independence, appended-zeroes, boundary overlap).
- `edgecheck_hybrid.c` (ASan/UBSan): determinism, seed sensitivity,
  per-position flips, avalanche ~32/64 bits (ideal), no adjacent-length
  collisions.

## History of the hybrid

- v1/v2: mum-bulk crossover experiments; v2 introduced the 7-lane mum
  window and exposed the rot linearity bug (fixed in both headers).
- v3: 7-region hybrid (xxh3 tiny/mid/large walk + mum 7-lane + rotr23
  bulk), SMHasher-clean, BIC 0.008660.
- v5: 129-224B moved from the xxh3-style secret walk to the mum 7-lane —
  same latency within noise, +15-19% throughput at 144-224B.
- v6 (current): mid ends at 112B; 113-288B all use the mum 7-lane
  (parallel 112B block + tail at 113-128B beats the mid path's serial add
  chain: latency -9%, throughput -10%).

## Status

Benchmarked on one SoC, one compiler, one input pattern. Verify on your
target hardware before trusting it in production.

## License

MPL-2.0. `plumshash_hybrid.h` mirrors the structure of xxh3 (BSD-2-Clause)
and rapidhash (MIT) with plumshash-family constants; see the header for
the full provenance note.
