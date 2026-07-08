# Auto-Generated Plan: pfdb.h v2 — PRIEMFORMULE Fractal Database

> **Mode:** standard
> **Quality standard:** Optimal performance, proper structure, clean code, clear docs.
> **Test Command:** `gcc -O3 -x c -DPFDB_IMPLEMENTATION -o pfdb pfdb.h -lpthread && rm -f data.pfdb && echo test | ./pfdb serve`

**Goal:** Single-header embedded fuzzy text search database using PRIEMFORMULE 9-column sieve as primary filter. On-disk via mmap.
**Tech Stack:** C11, pthreads, mmap, single-header `#define PFDB_IMPLEMENTATION` pattern
**Bug fix:** Heisenbug segfault at doc 6 (bloom loop) — caused by `__attribute__((always_inline))` forcing compiler to produce broken register allocation for the `pfdb_hash` FNV-1a loop at -O3 on aarch64. Fix: remove `always_inline`, add compiler barrier.

---

### Task 1: Identify and document bug
**Root cause analysis of Heisenbug:**
- Crash at doc 6 (always the same offset, consistent on every run)
- Adding `fprintf(stderr, ...)` before the bloom loop "fixes" it
- The `fprintf` is a function call that acts as a compiler barrier — forcing the compiler to reload `text` pointer from the stack instead of keeping it in a register
- Without the barrier, `-O3` combined with `__attribute__((always_inline))` on `pfdb_hash` produces incorrect register allocation/alias analysis
- `pfdb_hash` reads through `const uint8_t *data` which is aliased with `const char *text` — the compiler may incorrectly assume they don't alias and optimize away a reload
- **Fix:** Remove `__attribute__((always_inline))`, use plain `static inline`. Add `volatile` qualifier to bloom accumulator to force correct register reloads. Or just make `pfdb_hash` a non-inline `static` function.

### Task 2: Rewrite pfdb.h with correct architecture
**Files:**
- Rewrite: `pfdb.h`

**Changes from v1:**
1. Remove `PFDB_INLINE` (`__attribute__((always_inline))`) from hash functions — use plain `static`
2. Add compiler barrier comment and use `volatile` on bloom accumulator
3. Fix comment: "Trigram index × 9" → "Trigram index" (single index, DR is per-doc filter)
4. Remove unused `#include <stdbool.h>`
5. Add `#include <unistd.h>` and `#include <time.h>` for internal use
6. Clean up unused/unreachable code
7. Verify all `int` loop bounds are correct for string lengths

### Task 3: Write test program
**Files:**
- Create: `_test_pfdb3.c`
**Steps:**
1. Test: add 500 docs using word list (catches the Heisenbug)
2. Test: fuzzy search with typo
3. Test: substring search
4. Test: delete
5. No segfault

### Task 4: Final compile + cleanup
**Steps:**
1. `gcc -Wall -Wextra -Werror -O3 ...` — zero warnings
2. `gcc -O0 -g -fsanitize=address ...` — no ASAN errors
3. Clean up test files
4. Commit
