# PlumsHash v1.0.0 — build, test, and benchmark
# Test/bench sources need a C11 compiler; benches use -march=native.

CC       ?= clang
CFLAGS   ?= -O3 -march=native -Wall -Wextra
REF      := -Irefs -Irefs/t1ha -I.

TESTS    := test_sanitize test_quality test_comb16 test_security \
            smhasher_plums test_edge test_extensive
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

smhasher_plums: smhasher_plums.c plumshash.h
	$(CC) $(CFLAGS) -o $@ smhasher_plums.c -lm

test_edge: test_edge.c plumshash.h
	$(CC) $(CFLAGS) $(REF) -o $@ test_edge.c

test_extensive: test_extensive.c plumshash.h
	$(CC) $(CFLAGS) $(REF) -o $@ test_extensive.c

# ── benchmarks ───────────────────────────────────────────────────────────
bench_compete: bench_compete.c plumshash.h refs/rapidhash.h refs/wyhash.h refs/xxhash.h refs/t1ha/t1ha2.c
	$(CC) $(CFLAGS) $(REF) -o $@ bench_compete.c refs/t1ha/t1ha2.c

test_speed: test_speed.c plumshash.h
	$(CC) $(CFLAGS) $(REF) -o $@ test_speed.c

# ── gates ────────────────────────────────────────────────────────────────
# Quality gate: ASan/UBSan edge checks + quality suite + combination
# reproduction + security audit. Run `make test` after any change.
test: all
	ASAN_OPTIONS=detect_leaks=0 ./test_sanitize | tail -1
	./test_quality | tail -1
	./test_comb16 | grep -vcE 'distinct, maxgroup=1' || true
	./test_security | tail -1

bench: bench_compete test_speed
	./bench_compete
	./test_speed

clean:
	rm -f $(BINS)
