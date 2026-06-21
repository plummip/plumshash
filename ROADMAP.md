# fastdb — World's Fastest Columnar Database with Fuzzy Search

## Files
- `fastdb.c` — 1516 lines, single-file C11, zero deps beyond libc
- `fractal_portable.h` — CRC32-C (HW-accelerated ARM64/x86), mmap, NEON guard
- `Makefile` — build, test, bench

## Architecture
- **Schema-defined**: typed tables (INT, FLOAT, TEXT, BOOL)
- **Columnar**: each column is a contiguous mmap'd array → direct O(1) access
- **mmap'd**: MAP_SHARED + MADV_HUGEPAGE → zero-copy, TLB-efficient
- **Page-based**: 4096-byte pages, CRC32-C header integrity
- **Heap metadata**: TableMeta in heap survives mmap remap across grows

## Indexes (per column, built lazily)
1. **Hash** (FNV-1a, 251 slots/page, 4 pages) → O(1) equality
2. **Sorted** (qsort + binary search) → O(log n) range queries
3. **Trigram fuzzy** (FNV-1a hashing, 1000 buckets, 4 pages) → O(k) fuzzy
   - Myers bit-parallel edit distance (≤64 chars, ~5-10x vs Wagner-Fischer)
   - Damerau-Levenshtein fallback for transpositions
   - Two-pointer merge intersection (O(n+m) vs old O(n·log m))
   - qsort result ranking, stack-allocated buffers (zero heap in hot path)

## Prime Formula
```
n = 9r + c + 1 is prime <=> c ∈ {0,1,3,4,6,7} ∧ ∀p≤√n: r mod p ≠ r_p(c)
```
Built-in `komma` command: `fastdb komma 127` → decimal column classification.

## Performance (aarch64, 60K rows)
| Operation | Latency |
|-----------|---------|
| INT read (columnar) | 3.0 ns |
| Hash lookup | 3.0 ns |
| Range lower_bound | 19.1 ns |
| Range scan (50 rows) | 0.1 µs |
| Fuzzy search (ed≤2) | 31.2 µs |
| Insert (no sync) | 964 ns |
| Memory | 564 bytes/row |

## Commands
```
create TABLE col:TYPE ...
alter  TABLE add COL:TYPE / drop COL
insert TABLE val1 val2 ...
select TABLE [where COL = VAL] [range COL LO HI] [fuzzy COL PAT [K]] [prefix COL PAT] [search COL PAT]
delete TABLE where COL = VAL
import TABLE FILE.csv|.jsonl
tables
verify TABLE
bench [N]
komma N
serve    (interactive REPL)
```
