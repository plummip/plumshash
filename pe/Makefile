CC      = gcc
CFLAGS  = -std=c11 -Wall -Wextra -Wpedantic -Wshadow -O2
LDFLAGS = -pthread

# ── Targets ──────────────────────────────────────────

all: test_pe test_branch test_concur ped peshmd peshmc

# Library
pe.o: pe.c pe.h
	$(CC) $(CFLAGS) -c -o $@ $<

# Standalone test
test_pe: test_pe.c pe.o
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

# Branch test
test_branch: test_branch.c pe.o
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

# Concurrency stress test
test_concur: test_concur.c pe.o
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

# Unix-socket daemon
ped: ped.c pe.o
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

# Shared-memory daemon
peshmd: peshmd.c pe.o pe_shm.h
	$(CC) $(CFLAGS) -o $@ $< pe.o $(LDFLAGS)

# Shared-memory client
peshmc: peshmc.c pe_shm.h
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

# ── Phonies ──────────────────────────────────────────

test: test_pe test_branch test_concur
	./test_pe
	./test_branch
	./test_concur

clean:
	rm -f pe.o test_pe test_branch test_concur ped peshmd peshmc

.PHONY: all test clean
