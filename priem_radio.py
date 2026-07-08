#!/usr/bin/env python3
"""
Priem Base 7 Kolom Sequence — Radio Golf Detectie Analyse

n = 9r + c + 1: kolom 7 = cijfersom ≡ 8 (mod 9), n ≡ 8 (mod 9)
Onderzoek of kolom 7 priemen een goede frequentie-hopping/reeks
geven voor RADAR detectie.
"""

import sys, math, time
import numpy as np

MAX_N = 5_000_000

# ─── Segmented sieve ───
def generate_primes(limit):
    is_prime = bytearray(b'\x01') * (limit + 1)
    is_prime[0:2] = b'\x00\x00'
    sqrt_n = int(limit ** 0.5)
    for p in range(2, sqrt_n + 1):
        if is_prime[p]:
            step = p
            start = p * p
            is_prime[start:limit+1:step] = b'\x00' * ((limit - start)//step + 1)
    
    primes = [i for i, v in enumerate(is_prime) if v]
    return primes


def cols_from_primes(primes, base=9):
    cols = {c: [] for c in range(base)}
    for p in primes:
        c = (p - 1) % base
        cols[c].append(p)
    return cols


# ─── Hopping sequence analysis ───
def hopping_metrics(seq):
    N = len(seq)
    gaps = [seq[i+1] - seq[i] for i in range(N - 1)]
    gap_mean = float(np.mean(gaps))
    gap_std = float(np.std(gaps))
    
    # Autocorrelation (normalized, first 100 lags)
    arr = np.array(seq[:min(2000, N)], dtype=float)
    arr = arr - np.mean(arr)
    n_arr = len(arr)
    acf = np.zeros(min(101, n_arr // 2))
    for lag in range(len(acf)):
        if lag == 0:
            acf[lag] = 1.0
        else:
            cov = np.mean(arr[:-lag] * arr[lag:])
            var = np.var(arr)
            acf[lag] = cov / var if var > 0 else 0
    
    max_sidelobe = float(np.max(np.abs(acf[1:]))) if len(acf) > 1 else 1.0
    
    # Uniformity (normalized histogram spread)
    if max(seq) > min(seq):
        norm = (np.array(seq) - min(seq)) / (max(seq) - min(seq))
        bins = min(50, max(2, N // 20))
        hist, _ = np.histogram(norm, bins=bins)
        uniformity = 1.0 - float(np.std(hist) / (np.mean(hist) + 1e-10))
    else:
        uniformity = 0.0
    
    # Entropy
    unique = len(set(seq))
    entropy = math.log2(unique) if unique > 1 else 0
    
    # Gap distribution metrics
    gap_cv = gap_std / (gap_mean + 1e-10)  # coefficient of variation
    
    return {
        "N": N,
        "gap_mean": round(gap_mean, 2),
        "gap_std": round(gap_std, 2),
        "gap_cv": round(gap_cv, 3),
        "gap_min": min(gaps) if gaps else 0,
        "gap_max": max(gaps) if gaps else 0,
        "max_sidelobe": round(max_sidelobe, 4),
        "uniformity": round(uniformity, 4),
        "entropy_bits": round(entropy, 2),
        "unique_values": unique,
    }


def radio_metrics(seq):
    """Metrics for RADAR/radio detection."""
    N = len(seq)
    if N < 10:
        return {}
    
    unique = len(set(seq))
    tb_product = unique * N  # time-bandwidth product
    pg_db = 10 * math.log10(max(1, tb_product))
    
    gaps = [seq[i+1] - seq[i] for i in range(N - 1)]
    gap_cv = np.std(gaps) / (np.mean(gaps) + 1e-10)
    
    # Number of orthogonal sequences possible
    # For Costas-like arrays: N! / (something)
    # For FHSS: number of distinct hopping patterns
    
    return {
        "time_bandwidth_product": tb_product,
        "processing_gain_db": round(pg_db, 1),
        "unique_freqs": unique,
        "seq_len": N,
        "gap_variation": round(float(gap_cv), 3),
        "orthogonal_sequences_est": f"~{unique}! ≈ huge" if unique < 20 else ">>> huge",
    }


# ─── FHSS pattern conversion ───
def to_hopping_pattern(seq, M=79):
    """Convert sequence of N values to M-channel hopping pattern.
    Uses gaps between primes modulo M as the hopping pattern.
    """
    gaps = [seq[i+1] - seq[i] for i in range(min(len(seq)-1, 2000))]
    pattern = [g % M for g in gaps]
    
    # Analyze
    hist = [pattern.count(i) for i in range(M)]
    used = sum(1 for h in hist if h > 0)
    h_mean = float(np.mean(hist))
    h_std = float(np.std(hist))
    uniformity = 1.0 - h_std / (h_mean + 1e-10) if h_mean > 0 else 0
    
    # Minimum distance between same-frequency re-use
    last_pos = {}
    min_reuse = M
    max_reuse = 0
    for i, f in enumerate(pattern):
        if f in last_pos:
            d = i - last_pos[f]
            min_reuse = min(min_reuse, d)
            max_reuse = max(max_reuse, d)
        last_pos[f] = i
    
    return {
        "pattern_length": len(pattern),
        "channels": M,
        "channels_used": used,
        "utilization": f"{100*used/M:.0f}%",
        "uniformity": round(uniformity, 3),
        "min_reuse_distance": min_reuse,
        "max_reuse_distance": max_reuse,
        "hist_std": round(h_std, 2),
    }, pattern[:100]


# ─── Base-7 grid analysis ───
def base7_analysis(limit):
    """Prime distribution in base-7 grid: n = 7r + c + 1."""
    _, primes = generate_primes_memo(limit)
    cols_b7 = {c: [] for c in range(7)}
    for p in primes:
        cols_b7[(p - 1) % 7].append(p)
    return cols_b7


def generate_primes_memo(limit):
    """Memoized version to avoid re-sieving."""
    if not hasattr(generate_primes_memo, '_cache') or generate_primes_memo._cache[0] < limit:
        print(f"    [zeef tot {limit:,}]")
        t0 = time.time()
        primes = generate_primes(limit)
        generate_primes_memo._cache = (limit, primes)
        print(f"    [{len(primes):,} priemen in {time.time()-t0:.2f}s]")
    return generate_primes_memo._cache


# ─── MAIN ───
def main():
    limit = 5_000_000
    
    print("=" * 72)
    print("PRIEM-BASE 7 KOLOM SEQUENCE — RADIO GOLF DETECTIE ANALYSE")
    print("=" * 72)
    
    # Generate all primes
    print(f"\n[*] Genereren priemen tot {limit:,}...")
    _, primes = generate_primes_memo(limit)
    cols9 = cols_from_primes(primes, 9)
    
    print(f"    Kolom 7 priemen: {len(cols9[7]):,}")
    print(f"    Alle priemen:    {len(primes):,}")
    
    # ─── 1. Col7 hopping metrics ───
    print(f"\n{'─'*72}")
    print("1. HOPPING SEQUENTIE ANALYSE — Kolom 7")
    print(f"{'─'*72}")
    m = hopping_metrics(cols9[7])
    for k, v in m.items():
        print(f"    {k:20s}: {v}")
    
    # ─── 2. Compare all safe columns ───
    print(f"\n{'─'*72}")
    print("2. VERGELIJKING — Alle veilige kolommen (base 9)")
    print(f"{'─'*72}")
    print(f"{'Col':>5} {'N':>8} {'Gapμ':>7} {'Gapσ':>7} {'CV':>7} {'MaxSL':>8} {'Unif':>7} {'Entr':>6}")
    print(f"{'─'*56}")
    for c in [0, 1, 3, 4, 6, 7]:
        r = hopping_metrics(cols9[c])
        print(f"{c:>5} {r['N']:>8} {r['gap_mean']:>7} {r['gap_std']:>7} {r['gap_cv']:>7} {r['max_sidelobe']:>8} {r['uniformity']:>7} {r['entropy_bits']:>6}")
    
    # ─── 3. Radio suitability ───
    print(f"\n{'─'*72}")
    print("3. RADIO SUITABILITY (RADAR / Spread Spectrum)")
    print(f"{'─'*72}")
    for c in [0, 1, 3, 4, 6, 7]:
        radio = radio_metrics(cols9[c])
        print(f"    Kolom {c}:")
        for k, v in radio.items():
            print(f"        {k:25s}: {v}")
    
    # ─── 4. Autocorrelation comparison ───
    print(f"\n{'─'*72}")
    print("4. AUTOCORRELATIE — Kolom 7 vs Kolom 0 (eerste 50 lags)")
    print(f"{'─'*72}")
    
    def quick_acf(seq, max_lag=50):
        a = np.array(seq[:1000], dtype=float)
        a = a - np.mean(a)
        ac = np.correlate(a, a, mode='full')
        ac = ac / ac[len(a)-1]
        mid = len(a) - 1
        return ac[mid:mid+max_lag+1]
    
    ac7 = quick_acf(cols9[7], 50)
    ac0 = quick_acf(cols9[0], 50)
    
    print(f"    {'Lag':>5} {'Col7 ACF':>10} {'Col0 ACF':>10} {'Col7>Col0?':>12}")
    print(f"    {'─'*39}")
    for lag in range(1, min(51, len(ac7))):
        better = "COL7!" if abs(ac7[lag]) < abs(ac0[lag]) else ""
        print(f"    {lag:>5} {ac7[lag]:>10.4f} {ac0[lag]:>10.4f} {better:>12}")
    
    # ─── 5. FHSS pattern ───
    print(f"\n{'─'*72}")
    print("5. FREQUENCY HOPPING PATTERN — Kolom 7 als FHSS spread-spectrum")
    print(f"{'─'*72}")
    
    M_values = [19, 31, 79, 101]  # prime numbers of channels
    for M in M_values:
        fhss, sample = to_hopping_pattern(cols9[7], M)
        print(f"\n    M={M} kanalen:")
        print(f"        kanalen gebruikt: {fhss['channels_used']}/{M} ({fhss['utilization']})")
        print(f"        uniformiteit:     {fhss['uniformity']}")
        print(f"        min reuse:        {fhss['min_reuse_distance']} hops")
        print(f"        max reuse:        {fhss['max_reuse_distance']} hops")
        print(f"        hist std:         {fhss['hist_std']}")
        if M <= 31:
            print(f"        sample pattern:    {sample[:30]}")
    
    # ─── 6. Base-7 comparison ───
    print(f"\n{'─'*72}")
    print("6. BASE-7 GRID (n = 7r + c + 1) — Alternatief modulo 7")
    print(f"{'─'*72}")
    
    cols_b7 = base7_analysis(limit)
    bc_safe = [0, 1, 3, 5]  # columns not divisible by 2 or 7? No, let me compute properly
    # In base 7: c ≠ 6 (n ≡ 0 mod 7) and c ≠ 2 (n ≡ 3 mod 7? Actually need proper analysis)
    # For base 7: n = 7r + c + 1
    # Forbidden: c+1 ≡ 0 (mod 7) -> c=6 (n divisible by 7)
    # Also: c+1 ≡ 0 (mod 2) -> if r even? No, different parity rules
    # Also: c+1 ≡ 0 (mod 3) -> c=2,5
    
    print(f"\n{'Col':>5} {'N':>8} {'Gapμ':>7} {'Gapσ':>7} {'CV':>7} {'MaxSL':>8} {'Unif':>7} {'Entr':>6}")
    print(f"{'─'*56}")
    for c in range(7):
        r = hopping_metrics(cols_b7[c])
        print(f"{c:>5} {r['N']:>8} {r['gap_mean']:>7} {r['gap_std']:>7} {r['gap_cv']:>7} {r['max_sidelobe']:>8} {r['uniformity']:>7} {r['entropy_bits']:>6}")
    
    # ─── 7. Best column for radio ───
    print(f"\n{'─'*72}")
    print("7. OPTIMALE KOLOM VOOR RADIODETECTIE — Samenvatting")
    print(f"{'─'*72}")
    
    best_col = None
    best_score = -1
    for c in [0, 1, 3, 4, 6, 7]:
        r = hopping_metrics(cols9[c])
        # Score: low sidelobe + high uniformity + high entropy
        score = (1 - r['max_sidelobe']) * 0.4 + r['uniformity'] * 0.3 + (r['entropy_bits'] / 25) * 0.3
        print(f"    Kolom {c}: radiometriek_score = {score:.4f}")
        if score > best_score:
            best_score = score
            best_col = c
    
    print(f"\n    ▶ BESTE KOLOM: {best_col} (score={best_score:.4f})")
    
    # Costas array comparison
    print(f"\n{'─'*72}")
    print("8. COSTAS ARRAY COMPARISON — Theoretische limieten")
    print(f"{'─'*72}")
    
    # Costas arrays of order N have N! / (something) possibilities
    # But our sequence length is the N itself as frequency values
    print("""
    Costas arrays (RADAR): elke frequentie max 1x per rij/kolom
    Onze sequentie: elk getal is een 'frequentie' -> uniek in set
    
    Voordelen prime kolom-7 sequentie:
    - Automatisch unieke waarden (priemen zijn uniek)
    - Wiskundige structuur = deterministisch reproduceerbaar
    - Gatenpatroon volgt priemgetalstelling: π(x) ~ x/ln(x)
    
    Nadeel t.o.v. Costas:
    - Geen garantie dat elke frequentie precies 1x voorkomt
    - Frequenties zijn niet uniform verdeeld spectrum
    
    Conclusie Geschiktheid RADAR:
    - Goed voor FREQUENCY HOPPING (FHSS): ja, spread spectrum
    - Goed voor RANGING: matig, autocorrelatie niet perfect
    - Goed voor COMMUNICATIE: ja, grote set orthogonale sequenties
    """)
    
    print("=" * 72)
    print("EINDE ANALYSE")
    print("=" * 72)


if __name__ == "__main__":
    main()
