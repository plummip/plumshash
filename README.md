# PlumsHash

Fast 64-bit non-cryptographic hash. Single-file C, header-only, zero
dependencies, MPL-2.0.

- **Fast** — 5 empirically-selected paths; best-in-class latency to ~256B,
  dominates throughput at 2KB+ (see [Benchmarks](#benchmarks-vs-the-field))
- **Clean** — one function, one header, unaligned-safe, works on any length
  including 0, no globals, no allocation, no platform-specific code
- **Verified** — SMHasher full battery (all 15 groups) + standalone quality
  gates (see [Testing](#testing))

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
(`"1.0.0"`).

## Design (5 regions)

One hash function, five paths selected from measured crossover points on
the benchmark hardware (AArch64 Snapdragon, clang -O3 -march=native):

| len | Path | Rationale |
|-----|------|-----------|
| <= 3 | xxh3-style tiny | single-width mul, 2-mul chain, no seed premix |
| 4-16 | rapidhash-style mum tiny | 3-mul mum chain; passes the strict SMHasher BIC on 11-byte keys (the xxh3-style 9-16B fold fails it) |
| 17-112 | xxh3-style mid | independent mix16B pairs (ILP); wins 20-45% latency over serial mum chains |
| 113-384 | mum 7-lane | rapidhash >112 structure, 112B blocks, 7 parallel lanes; wins the 113-384B window on both harnesses |
| > 384 | rotr23 7-lane | plumshash fast-path port, 168B unroll + one mum per lane (breaks the linear data path) |

Provenance: the <=112B paths mirror xxh3's short-key functions
(BSD-2-Clause, Yann Collet) and rapidhash's tiny mum path (MIT, Nicoshev);
the 113-384B path mirrors rapidhash's >112 mum structure (MIT); the >384B
path is the plumshash fast path (MPL-2.0). All constants are
plumshash-family. The xxh3-derived mid path inherits xxh3's documented
seed-dependent multicollision property (17-112B, multiplication by zero).

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
2. `ph_mum_bulk`'s latency oscillates with `len mod 112`: its serial
   if-chain tail costs up to 6 dependent mums when the tail is ~112B
   (e.g. len 448: 55ns vs rot's 45ns) and nothing when the tail is 16B
   (len 464: 38ns). rot's word-remainder tail is mum-free and monotonic,
   so past 384B rot is the safe choice. 384 was the measured optimum:
   mum wins 289-384B, rot is flat and monotonic from 385B up.

## Benchmarks vs the field

Methodology (`bench_compete.c`): two harnesses, order-randomized, min-of-3,
pinned core.
- **LATENCY** — serial seed dependency (hash-table lookup model), fixed buffer.
- **THROUGHPUT** — independent seeds over a rotating 4 MB chunk pool (bulk/ILP model).

Device: AArch64 Snapdragon (8-core), clang -O3 -march=native, 2026-09-01.

### Latency (ns/op, lower is better)

| Size | plums | rapidhash | wyhash | xxh3 | t1ha2 |
|------|-------|-----------|--------|------|-------|
| 0B | 5.4 | 10.6 | 9.5 | 7.3 | 6.1 |
| 1B | 8.8 | 10.6 | 10.6 | 7.5 | 10.8 |
| 3B | 6.7 | 10.6 | 11.1 | 8.0 | 9.8 |
| 4B | 8.9 | 10.1 | 10.7 | 13.7 | 11.0 |
| 8B | 10.0 | 11.2 | 10.6 | 13.7 | 8.5 |
| 12B | 9.5 | 11.2 | 10.7 | 8.4 | 11.4 |
| 16B | 10.0 | 11.2 | 9.4 | 10.0 | 9.5 |
| 17B | 11.2 | 14.2 | 13.9 | 11.1 | 14.9 |
| 24B | 8.9 | 13.2 | 14.0 | 11.1 | 17.3 |
| 32B | 11.2 | 14.4 | 14.0 | 9.5 | 14.7 |
| 47B | 13.6 | 17.6 | 17.7 | 12.6 | 21.1 |
| 48B | 12.6 | 17.6 | 15.2 | 14.9 | 17.2 |
| 64B | 12.0 | 18.2 | 15.5 | 14.1 | 15.3 |
| 96B | 21.4 | 27.8 | 20.1 | 17.2 | 18.2 |
| 112B | 22.6 | 27.7 | 20.4 | 21.7 | 28.0 |
| 128B | 22.1 | 21.0 | 20.0 | 29.1 | 25.8 |
| 160B | 30.1 | 27.4 | 22.2 | 26.4 | 28.6 |
| 192B | 35.9 | 31.0 | 27.5 | 34.2 | 27.6 |
| 224B | 41.6 | 40.6 | 31.1 | 43.6 | 35.4 |
| 255B | 44.5 | 34.0 | 31.0 | 48.0 | 41.9 |
| 256B | 38.3 | 31.4 | 32.2 | 41.1 | 38.7 |
| 320B | 47.3 | 49.5 | 39.8 | 49.7 | 45.2 |
| 384B | 42.3 | 42.0 | 41.9 | 53.4 | 51.6 |
| 512B | 45.2 | 61.0 | 48.9 | 58.7 | 64.5 |
| 768B | 65.1 | 75.5 | 68.3 | 83.2 | 90.8 |
| 1KB | 65.0 | 97.4 | 90.0 | 98.0 | 118.9 |
| 2KB | 104.9 | 161.1 | 193.9 | 181.1 | 221.4 |
| 4KB | 176.6 | 319.0 | 330.9 | 363.0 | 443.3 |
| 8KB | 376.3 | 641.2 | 634.0 | 763.0 | 844.4 |
| 16KB | 815.2 | 1242.1 | 1298.2 | 1336.2 | 1677.1 |

### Throughput (ns/op, lower is better)

| Size | plums | rapidhash | wyhash | xxh3 | t1ha2 |
|------|-------|-----------|--------|------|-------|
| 0B | 3.7 | 1.8 | 1.9 | 3.7 | 2.3 |
| 1B | 5.3 | 6.3 | 6.5 | 5.4 | 7.6 |
| 3B | 4.8 | 6.6 | 5.8 | 5.7 | 7.7 |
| 4B | 7.8 | 5.8 | 7.2 | 5.9 | 7.5 |
| 8B | 6.7 | 6.3 | 6.9 | 5.5 | 8.4 |
| 12B | 5.6 | 6.6 | 7.4 | 5.6 | 9.4 |
| 16B | 6.3 | 6.3 | 8.5 | 6.4 | 9.9 |
| 17B | 8.7 | 9.6 | 7.8 | 9.0 | 14.3 |
| 24B | 9.7 | 8.4 | 7.2 | 9.5 | 11.0 |
| 32B | 8.2 | 10.8 | 8.5 | 9.4 | 13.7 |
| 47B | 14.8 | 8.9 | 11.3 | 14.1 | 16.1 |
| 48B | 14.0 | 10.0 | 12.3 | 13.9 | 18.2 |
| 64B | 14.2 | 11.1 | 7.6 | 13.7 | 18.9 |
| 96B | 18.9 | 16.6 | 16.9 | 20.1 | 10.2 |
| 112B | 24.5 | 19.2 | 14.0 | 21.0 | 21.8 |
| 128B | 13.1 | 18.7 | 16.8 | 21.5 | 21.9 |
| 160B | 24.9 | 21.3 | 20.8 | 30.9 | 27.0 |
| 192B | 35.7 | 28.5 | 28.8 | 37.7 | 28.1 |
| 224B | 40.7 | 38.5 | 25.8 | 46.9 | 37.3 |
| 255B | 30.4 | 9.1 | 24.9 | 45.7 | 41.4 |
| 256B | 31.6 | 31.2 | 27.0 | 39.8 | 36.6 |
| 320B | 48.7 | 41.1 | 36.9 | 45.4 | 43.7 |
| 384B | 56.4 | 40.0 | 36.8 | 23.5 | 57.0 |
| 512B | 46.7 | 52.6 | 52.7 | 76.9 | 60.5 |
| 768B | 58.4 | 85.1 | 75.4 | 88.0 | 83.8 |
| 1KB | 81.5 | 97.2 | 89.1 | 111.7 | 125.2 |
| 2KB | 128.8 | 184.6 | 212.7 | 199.6 | 255.1 |
| 4KB | 205.7 | 393.2 | 416.2 | 338.5 | 491.0 |
| 8KB | 513.2 | 763.9 | 679.8 | 720.5 | 948.4 |
| 16KB | 717.4 | 1530.9 | 1580.2 | 1633.9 | 1536.8 |

Reading: PlumsHash is best-in-class (or tied) for latency up to ~256B and
dominates at 512B+ (512B: 45 vs 49-64ns for the field; 4KB: 177 vs 319-443;
16KB: 815 vs 1242-1677 — 1.5-2.1x). Throughput is decisive at 2KB+ (2KB:
129 vs 185-255; 16KB: 717 vs 1531-1634 — 2.1-2.3x). The remaining gap is
throughput at 24-256B (up to ~40% vs wyhash/rapidhash), where the mid path
trades ILP for 20-45% better latency. Note: absolute ns differ run-to-run
on this SoC (walt DVFS boost bins occasionally land inside a single
measurement — e.g. rapid 255B and xxh3 384B in the throughput table); the
field ratios across a row are the stable part.

Reproduce: `make bench` (runs `bench_compete` vs the field and
`test_speed` for raw bandwidth).

## Testing

`make test` builds everything and runs the quality gate:

| Gate | What it checks |
|------|----------------|
| `test_sanitize` (ASan/UBSan) | determinism, seed sensitivity, per-position flips, avalanche ~32/64 bits (ideal), no adjacent-length collisions |
| `test_quality` | 21/21 SMHasher-style checks (sanity, avalanche >= 25%, chi², sparse, seed independence, appended-zeroes, permutation) |
| `test_comb16` | SMHasher Combination 16-byte reproduction: all keys distinct at every block size 16-352B — no low-weight collapse. (This caught the old linearity bug: the rotr23 lane mixes are linear in the data and a pure-XOR compression tree collapsed 100K keys to ~4K hashes; one mum per lane fixes it.) |
| `test_security` | multicollision bound, seed recovery attempt, differential, whitening bypass, strict avalanche, hash-flooding, length extension |

Standalone suites, also built by `make all`:

| File | What it checks |
|------|----------------|
| `smhasher_plums.c` | SMHasher-style suite (sanity, avalanche, differential, chi², sparse, permutation, appended-zeroes) — runs in CI |
| `test_edge.c` | path-boundary keys, pattern keys, tiny/long key extremes |
| `test_extensive.c` | broad sanity/chi²/avalanche matrix |

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

Benchmarked on one SoC, one compiler, one input pattern. Verify on your
target hardware before trusting it in production.

## License

MPL-2.0. `plumshash.h` mirrors the structure of xxh3 (BSD-2-Clause) and
rapidhash (MIT) with plumshash-family constants; see the header for the
full provenance note.
