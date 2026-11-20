# PlumsHash v1.2.0 — build, test, and benchmark
# Test/bench sources need a C11 compiler; benches use -march=native.

# `CC ?= clang` never fires: make predefines CC=cc, so ?= sees it as set and the
# documented clang default is silently ignored. This idiom replaces only make's
# built-in default — `CC=gcc make` and `make CC=gcc` both still win.
ifeq ($(origin CC),default)
CC       = clang
endif
CFLAGS   ?= -O3 -march=native -Wall -Wextra
REF      := -Irefs -Irefs/t1ha -I.

TESTS    := test_sanitize test_quality test_comb16 test_security test_headers \
            smhasher_plums test_edge test_extensive test_hybrid test_hybrid_security \
            test_hybrid_lifecycle
BENCHES  := bench_compete test_speed
BINS     := $(TESTS) $(BENCHES)

.PHONY: all test bench clean

all: $(BINS)

# ── tests ────────────────────────────────────────────────────────────────
test_sanitize: test_sanitize.c plumshash.h
	$(CC) -O1 -g -fsanitize=address,undefined -o $@ test_sanitize.c

test_quality: test_quality.c plumshash.h
	$(CC) $(CFLAGS) -o $@ test_quality.c -lm

test_comb16: test_comb16.c plumshash.h refs/rapidhash.h refs/xxhash.h
	$(CC) $(CFLAGS) $(REF) -o $@ test_comb16.c

test_security: test_security.c plumshash.h
	$(CC) -O2 $(REF) -o $@ test_security.c

# Both headers in one TU — the v1.2.0 integration path (see test_headers.c).
test_headers: test_headers.c plumshash.h hybrid_intern.h
	$(CC) $(CFLAGS) -pthread -o $@ test_headers.c

smhasher_plums: smhasher_plums.c plumshash.h
	$(CC) $(CFLAGS) -o $@ smhasher_plums.c -lm

test_edge: test_edge.c plumshash.h
	$(CC) $(CFLAGS) $(REF) -o $@ test_edge.c

test_extensive: test_extensive.c plumshash.h
	$(CC) $(CFLAGS) $(REF) -o $@ test_extensive.c

test_hybrid: test_hybrid.c plumshash.h hybrid_intern.h
	$(CC) $(CFLAGS) -pthread -o $@ test_hybrid.c

test_hybrid_security: test_hybrid_security.c plumshash.h hybrid_intern.h
	$(CC) -O1 -g -march=native -pthread -fsanitize=address,undefined \
		-fno-sanitize-recover=all -o $@ test_hybrid_security.c

# Slot lifecycle: evict/epoch delete semantics (tombstones), dedup across a
# deleted slot, and churn that must not exhaust the table.
test_hybrid_lifecycle: test_hybrid_lifecycle.c plumshash.h hybrid_intern.h
	$(CC) -O1 -g -march=native -pthread -fsanitize=address,undefined \
		-fno-sanitize-recover=all -o $@ test_hybrid_lifecycle.c

# ── benchmarks ───────────────────────────────────────────────────────────
bench_compete: bench_compete.c plumshash.h refs/rapidhash.h refs/wyhash.h refs/xxhash.h refs/t1ha/t1ha2.c
	$(CC) $(CFLAGS) $(REF) -o $@ bench_compete.c refs/t1ha/t1ha2.c

test_speed: test_speed.c plumshash.h
	$(CC) $(CFLAGS) $(REF) -o $@ test_speed.c

# ── gates ────────────────────────────────────────────────────────────────
# Quality gate: ASan/UBSan edge checks + quality suite + combination
# reproduction + security audit + intern-pool stress. Run after any change.
# comb16 is checked explicitly: 3 variants x 22 sizes = 66 data rows, and each
# must be fully distinct (maxgroup=1). The old `grep -vcE` form also counted
# the three "-- variant --" headers, so it printed 3 on success and could not
# fail on an empty/errored run.
test: all
	ASAN_OPTIONS=detect_leaks=0 ./test_sanitize | tail -1
	./test_quality | tail -1
	@out=$$(./test_comb16); \
	 rows=$$(printf '%s\n' "$$out" | grep -c 'maxgroup='); \
	 bad=$$(printf '%s\n' "$$out" | grep 'maxgroup=' | grep -vc 'distinct, maxgroup=1' || true); \
	 if [ "$$rows" -eq 66 ] && [ "$$bad" -eq 0 ]; then echo "  comb16: $$rows rows, 0 not fully distinct"; \
	 else { echo "  FAIL: comb16 rows=$$rows (want 66) bad=$$bad (want 0)"; printf '%s\n' "$$out" | grep -v 'distinct, maxgroup=1'; exit 1; }; fi
	./test_security | tail -1
	./test_headers | tail -1
	./test_edge | tail -1
	./test_extensive | grep -E 'RESULTS' | tail -1
	./test_hybrid | tail -1
	ASAN_OPTIONS=detect_leaks=0 ./test_hybrid_security | tail -1
	ASAN_OPTIONS=detect_leaks=0 ./test_hybrid_lifecycle | tail -1

# Benchmarks. `bench` writes the published min-of-7 tables to bench_results.txt
# (gitignored) and then raw bandwidth from test_speed. Override the run count
# or pin to a core: make bench RUNS=5 PIN=0
RUNS ?=
PIN  ?=
bench: bench_compete test_speed
	python3 bench_min7.py $(if $(RUNS),--runs $(RUNS),) $(if $(PIN),--pin $(PIN),) > bench_results.txt
	@echo "  -> bench_results.txt (tables)"
	./test_speed

clean:
	rm -f $(BINS)
