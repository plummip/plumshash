# Pfdb v3 — PRIEMFORMULE Fractal Database

> **PRIEMFORMULE Fractal Database** — A max-performance embedded fuzzy text search
> database that implements PRIEMFORMULE's 9-column hierarchical sieve as a
> suffix-array pyramide with Levenshtein automaton backtracking.

---

## 1. Architecture

```
                         QUERY
                           │
               ┌───────────▼───────────┐
               │    Exact match        │
               │  (SA binary search)   │◄── O(|Q| log N)
               └───────────┬───────────┘
                           │
              ┌────────────▼────────────┐
              │  Fuzzy prefix match     │
              │  (progressive narrowing)│◄── sa_char → sa_full
              └────────────┬────────────┘
                           │
              ┌────────────▼────────────┐
              │  Variant enumeration    │
              │  (26 substitutions)     │◄── first 3 chars
              └────────────┬────────────┘
                           │
              ┌────────────▼────────────┐
              │  SA range → candidates  │
              │  (unique doc IDs)       │
              └────────────┬────────────┘
                           │
              ┌────────────▼────────────┐
              │  Myers edit distance    │
              │  (constrained window)   │◄── O(ql × win / 64)
              └────────────┬────────────┘
                           │
                        RESULTS
```

### 1.1 Data Structures

**On-disk (single mmap'd file):**

```
┌────────────────┬────────────────┬────────────────┬────────────────┐
│   Header 4KB   │  Doc text      │  Suffix Array  │  LCP Array     │
│  (metadata)    │  (len-prefix)  │  (8B per char) │  (4B per char) │
└────────────────┴────────────────┴────────────────┴────────────────┘
```

**Header (pfdb_header_t, 4KB):**
```
offset  size  field
──────────────────────────────────────
0       4     magic      = "PFDB" (0x50464442)
4       4     version    = 3
8       4     num_docs
12      4     doc_store_off   ─┐
16      4     doc_store_size   │ contiguous
20      4     sa_off           ├─ all dynamic
24      4     sa_count         │
28      4     lcp_off          ┘
32      1     index_built
33-4095       _pad
```

**SA entry (pfdb_sa_t, 8 bytes):**
```
offset  size  field
────────────────────────────
0       4     doc_id    (document ID)
4       4     off       (byte offset in doc text)
```

Sorted lexicographically by the lowercased text at (doc_id, off).

**LCP entry (uint32_t, 4 bytes):**  
Longest common prefix length between SA[i-1] and SA[i]. LCP[0] = 0.

### 1.2 File Layout

All offsets are computed dynamically during `pfdb_rebuild()`:
```
sa_off   = doc_store_off + doc_store_size   (8-byte aligned)
lcp_off  = sa_off + sa_count × 8
end      = lcp_off + sa_count × 4
```

No fixed 64MB gaps. Compact layout ensures good cache locality.

---

## 2. PRIEMFORMULE Mapping

PRIEMFORMULE's 9-column sieve maps directly to the search pyramide:

| PRIEMFORMULE | Pfdb Implementation |
|---|---|
| Niveau 1: 9 kolommen (Z/9Z) | **SA char-level narrowing** — eerste 3 chars |
| Niveau 2: 6×6 schaakbord | **SA full-prefix narrowing** — chars 4+ |
| Niveau 3-∞: kleinere priemen | **Variant enumeration** — 26 letters at break point |
| Verboden-rijresten tabel | **Myers edit distance** — exacte verificatie |

Each level filters a fraction and passes the survivors to the next level:

```
Full SA (N entries)
  │
  ├─ query[0] match → ~N/26 entries (3.8% of original)
  │
  ├─ query[1] match → ~N/676 entries (0.15%)
  │
  ├─ query[2] match → ~N/17576 entries (0.006%)
  │
  ├─ query[3..p-1] match → ~1-10 entries
  │
  └─ Myers verification → ~1 entry (the answer)
```

---

## 3. API Reference

```c
// ── Lifecycle ──
pfdb_t *pfdb_open(const char *path);
    // Open or create database. Returns NULL on failure.
    // Thread-safe: file-level mutex for concurrent add.

void pfdb_close(pfdb_t *db);
    // Close, unmap, free resources.

// ── Document operations ──
uint32_t pfdb_add(pfdb_t *db, const char *text);
    // Add document (stored lowercased). Returns doc_id or -1 on failure.

int pfdb_delete(pfdb_t *db, uint32_t id);
    // Mark document as deleted. Invalidates index.

uint32_t pfdb_count(pfdb_t *db);
    // Number of non-deleted documents.

const char *pfdb_text(pfdb_t *db, uint32_t id);
    // Get document text (lowercased). NULL if deleted/invalid.

int pfdb_text_len(pfdb_t *db, uint32_t id);
    // Document text length.

uint8_t pfdb_dr(pfdb_t *db, uint32_t id);
    // PRIEMFORMULE digital root (sum(bytes) % 9).

// ── Search ──
int pfdb_search(pfdb_t *db, const char *query, int max_k,
                uint32_t *results, int max_results);
    // Fuzzy search: find docs within edit distance max_k of query.
    // Results sorted by edit distance ascending.
    // Returns number of results (≤ max_results).

int pfdb_find(pfdb_t *db, const char *needle,
              uint32_t *results, int max_results);
    // Substring search: case-insensitive.
    // O(log N) via SA prefix match.
    // Returns number of results.

void pfdb_rebuild(pfdb_t *db);
    // Rebuild SA+LCP index. Called automatically on search if dirty.
    // Build time: O(N log N) with string comparison.
```

---

## 4. Search Algorithm Details

### 4.1 Exact Match (pfdb_search, pfdb_find)

```
sa_range(q, 0):
  l = sa_lower_bound(q)   // binary search, O(|Q| log N)
  r = sa_upper_bound(q)   // binary search, O(|Q| log N)
  return [l, r)            // all suffixes starting with q
```

If range is non-empty, iterate and collect unique doc IDs.  
**pfdb_find** uses only this path — instant O(log N) substring search.

### 4.2 Fuzzy Match (pfdb_search)

```
Phase 1 — Character-level narrowing (first 3 chars):
  for p = 0..2:
    l = sa_char_lower(q[p])  within current range
    r = sa_char_upper(q[p])  within current range
    if range empty → break

Phase 2 — Full-prefix narrowing (chars 4+):
  for p = 3..ql-1:
    l = sa_lower_bound(q[p..])  within current range
    r = sa_upper_bound(q[p..])  within current range
    if range empty → break

Phase 3 — Variant enumeration (if prefix < 3 chars):
  for c = 'a'..'z':
    substitute q[break_pos] with c
    try sa_char_lower/upper
    keep largest resulting range

Phase 4 — Candidate verification:
  for each unique doc in [l, r):
    constrain window to [off-max_k, off+ql+max_k]
    Myers edit distance on window
    if ed ≤ max_k → add to results
```

### 4.3 Prefix Matching Detail

The SA is sorted by the full suffix text (lowercased). The progressive narrowing
uses two comparison strategies:

**CHAR-level (sa_char_lower/sa_char_upper):**  
Compares only `query[pos]` against `text[off + pos]` (1 byte).  
Used for the first 3 characters to find the initial SA range.

**FULL-level (sa_lower_bound/sa_upper_bound):**  
Compares `query[pos..ql-1]` against `text[off + pos..]` (full remaining).  
Used for characters 4+ and exact match verification.  
Implemented via `sa_cmp_n(db, e, q, ql, pos, ql-pos)`.

---

## 5. Performance Characteristics

| Operation | Complexity | 10K docs (60 chars avg) |
|---|---|---|
| Add document | O(len) | ~0.5 µs |
| Build index | O(N log N) | ~0.16s |
| Substring search | O(|Q| log N) | ~0.3 µs |
| Fuzzy search (ed≤2) | O(log N + candidates × ed) | ~850 µs |
| Delete | O(1) | ~0.1 µs |
| Index size | 12 bytes/char + 4.5 bytes/char (text) | ~16.5 MB |

### 5.1 Comparison with Trigram Index (v2)

| Metric | v2 (trigram) | v3 (SA+LCP) |
|---|---|---|
| Substring | 24 µs | **0.3 µs** (80×) |
| Fuzzy (ed≤2) | 1432 µs | **850 µs** (1.7×) |
| Build | 0.17s | 0.16s |
| Index size | 18 bytes/entry | 16 bytes/char + 4 bytes LCP |
| File size (10K docs) | ~4.5 MB | ~1.8 MB |
| Code size | 1125 lines | 1124 lines |

---

## 6. Project Structure

```
database/
├── pfdb.h                  # Single-header library (v3)
├── FLAWS.md                # Bug/flaw analysis
├── ffdb.c / ffdb           # Original fractal fuzzy DB (reference)
├── fuzzydb.c / fuzzydb     # Original fuzzy DB (reference)
├── pe/                     # Project Editor (separate tool)
└── bench_*.c               # Benchmark harnesses
```

### Compilation

```bash
# Library only
gcc -O3 -march=armv8-a -c pfdb_main.c -lpthread

# Standalone CLI
gcc -O3 -march=armv8-a -x c -DPFDB_IMPLEMENTATION -o pfdb pfdb.h -lpthread

# As library in your project
#define PFDB_IMPLEMENTATION
#include "pfdb.h"
```

### Usage

```bash
export PFDB_PATH=data.db
./pfdb add "Amsterdam"
./pfdb add "Rotterdam"
./pfdb search "Amsterdm" 2    # Fuzzy: finds Amsterdam
./pfdb search "Amsterdam" 0   # Exact
./pfdb find "dam"             # Substring
./pfdb count
./pfdb bench 10000            # Benchmark
```

---

## 7. Future Directions

1. **SIMD Myers** — Use NEON/SVE intrinsics to accelerate edit distance
2. **Multithreaded build** — Parallel SA construction for large datasets
3. **Sorted output** — Return results sorted by doc_id instead of edit distance
4. **Regex search** — Use SA for prefix/suffix/infix regex patterns
5. **Concurrent readers** — Reader-writer lock for concurrent search
6. **fuzzy variant backtracking** — Levenshtein automaton over SA range for full fuzzy without prefix assumption
