# fastdb — world's fastest schema-defined columnar database

Columnar database with hash, sorted, and trigram-fuzzy indexes.
Built on PlumsHash (ARX hash). mmap-backed, instant startup.

## Quick start

```sh
gcc -O3 -march=armv8-a -o fastdb fastdb.c -I.
./fastdb serve
```

## Commands

```
create TABLE col:TYPE ...        Create table with typed columns
alter  TABLE add COL:TYPE        Add column
alter  TABLE drop COL            Remove column
insert TABLE val1 val2 ...       Insert row
select TABLE [where COL = VAL] [range COL LO HI]
              [search COL PAT] [fuzzy COL PAT [K]] [prefix COL PAT]
delete TABLE where COL = VAL     Soft-delete rows
import TABLE FILE                Bulk import CSV/JSONL
tables                           List all tables
verify TABLE                     Check data integrity
bench [N]                        Run benchmark
komma N                          Decimal column classification
serve                            Interactive REPL
```

## Column types

- INT (4 byte)
- FLOAT (8 byte)
- TEXT (256-byte slots)
- BOOL (1 byte)

## Indexes per column

- **Hash** — PlumsHash ARX, O(1) equality
- **Sorted** — qsort + binary search, O(log n) range queries
- **Trigram fuzzy** — PlumsHash + Myers bit-parallel edit distance + Damerau fallback

## License

MPL 2.0

## Dependencies

- `plumshash.h` — PlumsHash (bundled copy from ../plumshash/)
- `fractal_portable.h` — portable primitives (bundled)
