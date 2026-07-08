/*
 * sieve_primes.h — Fast prime generation using the 9-column sieve.
 *
 * From the PRIEMFORMULE: n = 9r + c + 1 is prime only if c ∈ {0,1,3,4,6,7}
 * (safe columns). This eliminates 33% of candidates instantly with (n-1) % 9.
 *
 * Combined with Miller-Rabin for verification. Useful for:
 *   - Hash table sizing (next_prime(N))
 *   - Bloom filter parameters
 *   - The komma command in fastdb
 */

#ifndef SIEVE_PRIMES_H
#define SIEVE_PRIMES_H

#include <stdint.h>

/* ── Fast pre-check: column sieve (no division) ── */
static inline int sieve9_check(uint64_t n) {
    if (n < 2) return 0;
    if (n == 2 || n == 3) return 1;
    uint32_t c = (n - 1) % 9;
    /* Safe columns: c ∈ {0,1,3,4,6,7} */
    return (c == 0 || c == 1 || c == 3 || c == 4 || c == 6 || c == 7);
}

/* ── Miller-Rabin deterministic for 64-bit (bases from Jim Sinclair) ── */
static uint64_t sieve_mulmod(uint64_t a, uint64_t b, uint64_t m) {
    /* a * b mod m without overflow */
    uint64_t r = 0;
    while (b) {
        if (b & 1) { r += a; if (r >= m) r -= m; }
        a += a; if (a >= m) a -= m;
        b >>= 1;
    }
    return r;
}

static uint64_t sieve_powmod(uint64_t a, uint64_t e, uint64_t m) {
    uint64_t r = 1;
    while (e) {
        if (e & 1) r = sieve_mulmod(r, a, m);
        a = sieve_mulmod(a, a, m);
        e >>= 1;
    }
    return r;
}

static int miller_rabin(uint64_t n) {
    if (n < 2) return 0;
    if (n == 2 || n == 3) return 1;
    if ((n & 1) == 0) return 0;

    /* Write n-1 = d * 2^s */
    uint64_t d = n - 1;
    int s = 0;
    while ((d & 1) == 0) { d >>= 1; s++; }

    /* Bases sufficient for n < 2^64 (deterministic) */
    static const uint64_t bases[] = {2, 325, 9375, 28178, 450775, 9780504, 1795265022};
    int nbases = 7;

    for (int i = 0; i < nbases; i++) {
        uint64_t a = bases[i] % n;
        if (a == 0) continue;

        uint64_t x = sieve_powmod(a, d, n);
        if (x == 1 || x == n - 1) continue;

        int composite = 1;
        for (int r = 0; r < s - 1; r++) {
            x = sieve_mulmod(x, x, n);
            if (x == n - 1) { composite = 0; break; }
        }
        if (composite) return 0;
    }
    return 1;
}

/* ── Combined fast prime test: column sieve + Miller-Rabin ── */
static inline int sieve_is_prime(uint64_t n) {
    if (!sieve9_check(n)) return 0;
    return miller_rabin(n);
}

/* ── Next prime ≥ n ── */
static uint64_t sieve_next_prime(uint64_t n) {
    if (n <= 2) return 2;
    if (n <= 3) return 3;
    if (n <= 5) return 5;

    /* Start from n, skip to next safe column */
    while (!sieve9_check(n)) n++;

    /* Search */
    while (1) {
        /* Column sieve: only test numbers in safe columns */
        uint32_t c = (n - 1) % 9;
        /* Step pattern for safe columns: 0→1 (+1), 1→3 (+2), 3→4 (+1),
           4→6 (+2), 6→7 (+1), 7→0 (+2) — alternating +1,+2 */
        static const uint32_t step[9] = {1, 2, 0, 1, 2, 0, 1, 2, 0};

        if (miller_rabin(n)) return n;
        n += step[c];
    }
}

#endif
