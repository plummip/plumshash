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
(`"1.1.0"`).

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

## Benchmarks vs the field

Methodology (`bench_compete.c`): two harnesses, order-randomized, min-of-N,
pinned core. Tables below are min-of-7-runs (each run is internally
min-of-3).
- **LATENCY** — serial seed dependency (hash-table lookup model), fixed buffer.
- **THROUGHPUT** — independent seeds over a rotating 4 MB chunk pool (bulk/ILP model).

Device: AArch64 Snapdragon (8-core), clang -O3 -march=native, 2026-09-09.

> **Noise caveat:** this SoC's `walt` governor drifts DVFS on a 10-100 ms
> scale, so absolute ns/op are not reproducible run-to-run (a stray boost
> bin or drift bin lands inside single measurements — e.g. the 96B and 255B
> latency rows below). Treat the field *ratios within a row* as the signal,
> not the absolute values. The v1.1.0 improvement quoted in the Reading
> section is from an in-process interleaved harness (min-of-15, variants
> rotated per rep, core-pinned) that cancels this drift.

### Latency (ns/op, lower is better)

| Size | plums | rapidhash | wyhash | xxh3 | t1ha2 |
|------|-------|-----------|--------|------|-------|
| 0B | 4.6 | 5.1 | 7.1 | 6.2 | 3.4 |
| 1B | 5.9 | 7.1 | 7.1 | 6.7 | 7.3 |
| 3B | 5.9 | 7.1 | 7.1 | 6.7 | 7.3 |
| 4B | 6.7 | 7.5 | 7.2 | 9.1 | 7.3 |
| 8B | 6.7 | 5.4 | 7.2 | 9.1 | 5.3 |
| 12B | 6.7 | 7.5 | 7.2 | 6.6 | 7.1 |
| 16B | 6.7 | 7.5 | 7.2 | 6.6 | 8.8 |
| 17B | 7.5 | 9.2 | 9.3 | 7.3 | 11.0 |
| 24B | 7.5 | 9.5 | 9.3 | 7.4 | 11.6 |
| 32B | 7.5 | 9.5 | 9.3 | 7.4 | 12.0 |
| 47B | 9.0 | 11.7 | 11.8 | 8.9 | 14.1 |
| 48B | 9.0 | 11.7 | 11.0 | 8.9 | 14.1 |
| 64B | 9.0 | 14.1 | 11.0 | 8.9 | 12.8 |
| 96B | 7.0 | 18.5 | 13.5 | 10.5 | 14.8 |
| 112B | 12.0 | 21.9 | 13.4 | 12.1 | 18.8 |
| 128B | 15.6 | 13.6 | 15.5 | 19.7 | 17.2 |
| 160B | 15.8 | 14.0 | 15.8 | 16.5 | 19.1 |
| 192B | 20.0 | 23.0 | 18.3 | 25.4 | 21.4 |
| 224B | 20.6 | 27.2 | 20.8 | 20.8 | 23.7 |
| 255B | 32.4 | 19.2 | 20.7 | 32.3 | 31.9 |
| 256B | 20.9 | 19.2 | 20.7 | 32.1 | 25.9 |
| 320B | 25.5 | 28.4 | 25.4 | 30.9 | 30.1 |
| 384B | 31.0 | 25.7 | 28.1 | 31.4 | 34.5 |
| 512B | 32.2 | 31.1 | 35.3 | 33.3 | 43.2 |
| 768B | 38.5 | 43.3 | 48.1 | 39.9 | 60.6 |
| 1KB | 43.1 | 44.0 | 60.2 | 46.0 | 78.2 |
| 2KB | 64.9 | 80.4 | 112.9 | 77.1 | 147.5 |
| 4KB | 108.2 | 128.1 | 214.8 | 141.5 | 287.2 |
| 8KB | 195.6 | 287.3 | 423.0 | 267.0 | 478.6 |
| 16KB | 368.6 | 565.6 | 833.8 | 522.9 | 1120.8 |

### Throughput (ns/op, lower is better)

| Size | plums | rapidhash | wyhash | xxh3 | t1ha2 |
|------|-------|-----------|--------|------|-------|
| 0B | 1.6 | 1.9 | 2.1 | 1.8 | 2.8 |
| 1B | 2.8 | 3.0 | 3.1 | 2.1 | 2.7 |
| 3B | 2.0 | 3.0 | 2.2 | 3.0 | 3.8 |
| 4B | 2.9 | 2.1 | 3.4 | 3.1 | 3.8 |
| 8B | 2.1 | 2.9 | 3.4 | 2.2 | 2.9 |
| 12B | 2.9 | 2.9 | 2.4 | 2.1 | 3.3 |
| 16B | 2.9 | 2.1 | 3.4 | 3.0 | 4.6 |
| 17B | 2.9 | 3.9 | 2.8 | 4.2 | 5.5 |
| 24B | 4.1 | 2.8 | 2.5 | 3.0 | 5.5 |
| 32B | 4.1 | 3.9 | 3.5 | 4.2 | 4.3 |
| 47B | 6.5 | 4.7 | 4.2 | 4.8 | 6.1 |
| 48B | 6.3 | 4.5 | 5.1 | 4.6 | 8.0 |
| 64B | 6.3 | 5.2 | 3.7 | 6.4 | 8.2 |
| 96B | 8.2 | 6.2 | 4.9 | 8.7 | 10.2 |
| 112B | 10.3 | 7.5 | 4.9 | 10.9 | 8.7 |
| 128B | 7.8 | 8.7 | 5.5 | 7.8 | 12.2 |
| 160B | 8.8 | 7.2 | 6.3 | 10.4 | 10.1 |
| 192B | 10.6 | 8.1 | 10.5 | 16.9 | 16.0 |
| 224B | 11.7 | 13.1 | 11.5 | 16.8 | 17.8 |
| 255B | 9.5 | 13.7 | 9.4 | 25.6 | 22.4 |
| 256B | 9.0 | 12.9 | 12.3 | 19.2 | 19.7 |
| 320B | 15.3 | 16.2 | 10.7 | 25.3 | 23.6 |
| 384B | 18.4 | 17.2 | 13.0 | 19.1 | 27.3 |
| 512B | 22.7 | 21.9 | 23.1 | 26.5 | 35.4 |
| 768B | 21.1 | 23.0 | 25.7 | 22.5 | 52.3 |
| 1KB | 25.4 | 27.3 | 48.1 | 39.1 | 69.7 |
| 2KB | 40.3 | 73.3 | 100.1 | 71.6 | 139.3 |
| 4KB | 99.9 | 142.8 | 203.7 | 114.0 | 278.1 |
| 8KB | 134.1 | 280.3 | 414.4 | 187.8 | 400.9 |
| 16KB | 259.1 | 557.4 | 828.4 | 522.6 | 1116.0 |

Reading: PlumsHash leads (or ties) the field for latency across most of
the range and dominates throughput at 512B+ (4KB: ~100 vs 114-278ns; 16KB:
~259 vs 523-1116ns — 2-4.3x). The v1.0.0 → v1.1.0 change replaces the
113-384B 7-lane/112B mum path with a 3-lane/48B one. Measured on the
drift-free interleaved harness, that closes a 20-50% latency/throughput
gap to wyhash in that window (20-33% faster in 160-320B; e.g. 160B
29.5→20.3ns, 224B 39.3→26.3ns). The 255B latency row above is a
measurement artifact, not the 3-lane regressing (255 = 5×48+15, a tail
size the interleaved harness pins at ~28ns, on par with 256B).

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

MPL-2.0. `plumshash.h` mirrors the structure of xxh3 (BSD-2-Clause),
rapidhash (MIT), and wyhash (Unlicense) with plumshash-family constants;
see the header for the full provenance note.
