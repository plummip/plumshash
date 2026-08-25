# PlumsHash — build, test, and benchmark
# Test/bench sources need a C11 compiler; benches use -march=native.

CC       ?= clang
CFLAGS   ?= -O3 -march=native -Wall -Wextra
REF      := -Irefs -Irefs/t1ha -I. 

BINS     := quality_hybrid edgecheck_hybrid repro_comb16 verify_identical \
            bench_hybrid bench_compete security_test smhasher_plums \
            test_mini test_speed test_edge test_extensive

.PHONY: all test bench clean

all: $(BINS)

quality_hybrid: quality_hybrid.c plumshash_hybrid.h
	$(CC) $(CFLAGS) $(REF) -o $@ quality_hybrid.c -lm

edgecheck_hybrid: edgecheck_hybrid.c plumshash_hybrid.h
	$(CC) -O1 -g -fsanitize=address,undefined $(REF) -o $@ edgecheck_hybrid.c

repro_comb16: repro_comb16.c plumshash_hybrid.h
	$(CC) $(CFLAGS) $(REF) -o $@ repro_comb16.c

verify_identical: verify_identical.c t_old.c t_new.c plumshash_hybrid_v3.h plumshash_hybrid.h
	$(CC) $(CFLAGS) $(REF) -o $@ verify_identical.c t_old.c t_new.c

bench_hybrid: bench_hybrid.c plumshash.h plumshash_hybrid.h
	$(CC) $(CFLAGS) $(REF) -o $@ bench_hybrid.c

bench_compete: bench_compete.c plumshash.h plumshash_hybrid.h refs/rapidhash.h refs/wyhash.h refs/xxhash.h refs/t1ha/t1ha2.c
	$(CC) $(CFLAGS) $(REF) -o $@ bench_compete.c refs/t1ha/t1ha2.c

security_test: security_test.c plumshash.h
	$(CC) -O2 $(REF) -o $@ security_test.c

smhasher_plums: smhasher_plums.c plumshash.h
	$(CC) $(CFLAGS) $(REF) -o $@ smhasher_plums.c

test_mini: test_mini.c plumshash.h
	$(CC) $(CFLAGS) $(REF) -o $@ test_mini.c

test_speed: test_speed.c plumshash.h
	$(CC) $(CFLAGS) $(REF) -o $@ test_speed.c

test_edge: test_edge.c plumshash.h
	$(CC) $(CFLAGS) $(REF) -o $@ test_edge.c

test_extensive: test_extensive.c plumshash.h
	$(CC) $(CFLAGS) $(REF) -o $@ test_extensive.c

# Quality gate: hybrid regression + edge/quality/repro/security checks
test: all
	ASAN_OPTIONS=detect_leaks=0 ./edgecheck_hybrid | tail -1
	./quality_hybrid | tail -1
	./repro_comb16 | grep -vcE 'distinct, maxgroup=1' || true
	./verify_identical | tail -1
	./security_test | tail -1

bench: all
	./bench_hybrid
	./bench_compete

clean:
	rm -f $(BINS)
