#!/usr/bin/env python3
"""
costas_brein_sdr.py — Complete SDR-keten simulatie

9-kolom priem → Costas code → SDR zender → hoofdreflectie → SDR ontvanger → 
  Costas matched filter → detectie hersenactiviteit

Gebruik:
  python3 costas_brein_sdr.py [--p 701] [--band 500 1500] [--sigma 0.995]
"""

import sys, math, time, argparse
import numpy as np

# ═══════════════════════════════════════════
# 9-COLUMN PRIME TOOLS
# ═══════════════════════════════════════════

def zeef(limit):
    bp = bytearray(b'\x01') * (limit + 1)
    bp[0:2] = b'\x00\x00'
    for p in range(2, int(limit**0.5)+1):
        if bp[p]:
            bp[p*p:limit+1:p] = b'\x00' * ((limit-p*p)//p + 1)
    return [i for i, v in enumerate(bp) if v]

def factoriseer(n):
    f = set()
    d = 2
    while d*d <= n:
        if n % d == 0:
            f.add(d)
            while n % d == 0: n //= d
        d += 1 if d == 2 else 2
    if n > 1: f.add(n)
    return f

def prim_wortel(p):
    if p < 2: return None
    pf = factoriseer(p-1)
    for g in range(2, p):
        if all(pow(g, (p-1)//q, p) != 1 for q in pf):
            return g
    return None

def kolom_7_priemen(limit):
    alle = zeef(limit)
    return [p for p in alle if (p - 1) % 9 == 7]

# ═══════════════════════════════════════════
# COSTAS GENERATOR
# ═══════════════════════════════════════════

class CostasGenerator:
    """Converteert kolom-7 priem naar frequentie-hopping patroon."""
    
    def __init__(self, p, band_mhz=(500, 1500), hop_tijd_s=10e-6):
        self.p = p
        self.kolom = (p - 1) % 9
        self.alpha = prim_wortel(p)
        self.order = p - 1
        self.band_start, self.band_end = band_mhz
        self.hop_tijd_s = hop_tijd_s
        
        if not self.alpha:
            raise ValueError(f"Geen primitieve wortel voor p={p}")
        
        # Welch constructie: f[i] = α^(i+1) mod p
        self.freq_idx = []  # α^i mod p
        val = 1
        for _ in range(self.order):
            val = (val * self.alpha) % p
            self.freq_idx.append(val)
        
        # Map naar frequentieband
        self.freq_mhz = []
        for v in self.freq_idx:
            f = band_mhz[0] + v / (p - 1) * (band_mhz[1] - band_mhz[0])
            self.freq_mhz.append(f)
        
        self.tijd_s = [i * hop_tijd_s for i in range(self.order)]
        self.tb_product = self.order * self.order
        self.pg_db = 10 * math.log10(self.tb_product) if self.tb_product > 0 else 0
    
    def info(self):
        return {
            'p': self.p,
            'kolom': self.kolom,
            'alpha': self.alpha,
            'order': self.order,
            'band': (self.band_start, self.band_end),
            'tb_product': self.tb_product,
            'pg_db': round(self.pg_db, 1),
            'total_tijd_us': self.order * self.hop_tijd_s * 1e6,
        }


# ═══════════════════════════════════════════
# 5-LAAGS HOOFDMODEL (verbeterd)
# ═══════════════════════════════════════════

EPS0 = 8.854e-12
MU0 = 4 * math.pi * 1e-7
C0 = 299792458

# Cole-Cole parameters (Gabriel 1996): (eps_inf, sigma, dikte,
#   [(Δε1, τ1, α1), (Δε2, τ2, α2), (Δε3, τ3, α3), (Δε4, τ4, α4)])
WEEFSEL_PARAMS = {
    'skin':         (4.0, 0.0002, 0.003,
                     [(4, 7.96e-12, 0.1), (40, 15.92e-9, 0.15),
                      (1e4, 106.1e-6, 0.22), (2e6, 5.305e-3, 0.0)]),
    'skull':        (2.5, 0.0010, 0.007,
                     [(10, 13.26e-12, 0.2), (20, 79.58e-9, 0.2),
                      (100, 159.15e-6, 0.2), (5e4, 7.958e-3, 0.0)]),
    'csf':          (6.0, 2.0, 0.003,
                     [(55, 7.96e-12, 0.1), (100, 19.89e-9, 0.1),
                      (50, 79.58e-6, 0.1), (5e4, 4.774e-3, 0.0)]),
    'grey_matter':  (4.0, 0.02, 0.004,
                     [(45, 7.96e-12, 0.1), (400, 15.92e-9, 0.15),
                      (2e5, 106.1e-6, 0.22), (4e7, 5.305e-3, 0.0)]),
    'white_matter': (4.0, 0.02, 0.050,
                     [(32, 7.96e-12, 0.1), (100, 15.92e-9, 0.15),
                      (3e4, 106.1e-6, 0.22), (2e7, 5.305e-3, 0.0)]),
}

def cole_cole_eps(f, eps_inf, sigma, cole_params):
    """Bereken complexe permittiviteit via 4-Cole-Cole model."""
    w = 2 * math.pi * f
    eps = eps_inf + 0j
    for delta_e, tau, alpha in cole_params:
        eps += delta_e / (1 + (1j * w * tau) ** (1 - alpha))
    eps += sigma / (1j * w * EPS0)
    return eps

def hoofd_reflectie(f, grey_sigma_factor=1.0):
    """Totale reflectiecoefficient van 5-laags hoofd bij freq f (Hz).
    
    Gebruikt transmissielijn model: opeenvolgende impedantie-transformaties.
    Retourneert: |Γ| = magnitude van totale reflectie.
    """
    Z0 = 377.0  # karakteristieke impedantie vrije ruimte
    
    layer_names = ['skin', 'skull', 'csf', 'grey_matter', 'white_matter']
    Z_load = Z0  # laatste laag: aansluiting op vrije ruimte (oneindig dik)
    
    # Werk van achter naar voren (diepste laag eerst)
    for naam in reversed(layer_names):
        eps_inf, sigma, dikte, cole = WEEFSEL_PARAMS[naam]
        
        # Pas conductiviteit aan voor grijze stof
        if naam == 'grey_matter':
            sigma *= grey_sigma_factor
        
        eps = cole_cole_eps(f, eps_inf, sigma, cole)
        
        # Karakteristieke impedantie: Z_c = Z0 / sqrt(ε_r)
        Z_c = Z0 / (eps ** 0.5)
        
        # Propagatieconstante: γ = j * 2πf * sqrt(ε_r) / c
        gamma = 1j * (2 * math.pi * f / C0) * (eps ** 0.5)
        
        # Impedantietransformatie over laagdikte d:
        # Z_in = Z_c * (Z_load + Z_c * tanh(γ*d)) / (Z_c + Z_load * tanh(γ*d))
        tanh_gd = np.tanh(gamma * dikte)
        teller = Z_load + Z_c * tanh_gd
        noemer = Z_c + Z_load * tanh_gd
        Z_in = Z_c * teller / noemer
        
        Z_load = Z_in  # input = load voor volgende laag
    
    # Totale reflectie: Γ = (Z_in - Z0) / (Z_in + Z0)
    Gamma = (Z_load - Z0) / (Z_load + Z0)
    return abs(Gamma)


# ═══════════════════════════════════════════
# SDR KETEN SIMULATIE
# ═══════════════════════════════════════════

def sdr_keten_simulatie(costas, sigma_factor=1.0, fs=20e6, ruis_niveau=0.001):
    """
    Simuleer complete SDR keten:
      Zender → Costas-gemoduleerde draaggolf → Hoofdreflectie → Ontvanger
      
    Returns: (tx_signal, rx_signal, metadata)
    """
    n_hops = costas.order
    samples_per_hop = int(costas.hop_tijd_s * fs)
    total_samples = n_hops * samples_per_hop
    dt = 1.0 / fs
    
    tx = np.zeros(total_samples, dtype=np.complex128)
    rx = np.zeros(total_samples, dtype=np.complex128)
    
    reflecties = []
    
    for hop in range(n_hops):
        f_hz = costas.freq_mhz[hop] * 1e6
        f_idx = costas.freq_idx[hop]
        
        # Reflectiecoefficient bij deze frequentie
        R_base = hoofd_reflectie(f_hz, 1.0)  # rust
        R_act = hoofd_reflectie(f_hz, sigma_factor)  # actief
        reflecties.append((R_base, R_act, R_act - R_base))
        
        # Costas-modulatie: BPSK op fase = α^i mod p
        # De fase-modulatie codeert het Costas patroon
        phase_code = (2 * math.pi * f_idx) / costas.p
        
        for s in range(samples_per_hop):
            idx = hop * samples_per_hop + s
            t = s * dt
            
            # Zend signaal: cos(2πft + φ_code)
            tx[idx] = np.exp(1j * (2 * math.pi * f_hz * t + phase_code))
            
            # Ontvangen signaal: reflectie × zendsignaal + ruis
            # De reflectie is frequentie-afhankelijk
            rx[idx] = R_act * tx[idx]
        
        # Voeg ruis toe (thermisch + kwantizatie)
    ruis = (np.random.randn(total_samples) + 1j * np.random.randn(total_samples)) * ruis_niveau / math.sqrt(2)
    rx += ruis
    
    return tx, rx, reflecties


def costas_matched_filter(rx_signal, costas, fs=20e6):
    """
    Costas matched filter correlator.
    
    Genereert ideaal Costas-referentie signaal en correlatie met ontvangen signaal.
    Returns: correlatie-magnitude, genormaliseerd [0, 1]
    """
    n_hops = costas.order
    samples_per_hop = int(costas.hop_tijd_s * fs)
    total_samples = n_hops * samples_per_hop
    dt = 1.0 / fs
    
    if len(rx_signal) < total_samples:
        rx_signal = rx_signal[:total_samples]
    
    # Genereer referentie
    ref = np.zeros(total_samples, dtype=np.complex128)
    for hop in range(n_hops):
        f_hz = costas.freq_mhz[hop] * 1e6
        f_idx = costas.freq_idx[hop]
        phase_code = (2 * math.pi * f_idx) / costas.p
        for s in range(samples_per_hop):
            idx = hop * samples_per_hop + s
            t = s * dt
            ref[idx] = np.exp(1j * (2 * math.pi * f_hz * t + phase_code))
    
    # Correlatie
    corr = np.abs(np.sum(rx_signal * np.conj(ref)))
    norm = np.sqrt(np.sum(np.abs(rx_signal)**2) * np.sum(np.abs(ref)**2))
    
    return corr / (norm + 1e-15)


def autocorrelatie(costas, max_lag=50):
    """Bereken autocorrelatie van Costas hopping patroon."""
    freq = np.array(costas.freq_mhz)
    freq = freq - np.mean(freq)
    n = len(freq)
    
    acf = np.zeros(max_lag + 1)
    acf[0] = 1.0
    
    for lag in range(1, max_lag + 1):
        acf[lag] = np.sum(freq[:-lag] * freq[lag:]) / np.sum(freq**2)
    
    return acf


def kruiscorrelatie(costas_a, costas_b):
    """Bereken kruiscorrelatie tussen twee Costas codes."""
    n = min(len(costas_a.freq_mhz), len(costas_b.freq_mhz))
    a = np.array(costas_a.freq_mhz[:n])
    b = np.array(costas_b.freq_mhz[:n])
    a = a - np.mean(a)
    b = b - np.mean(b)
    return np.sum(a * b) / (np.sqrt(np.sum(a**2) * np.sum(b**2)) + 1e-15)


# ═══════════════════════════════════════════
# MAIN
# ═══════════════════════════════════════════

def main():
    parser = argparse.ArgumentParser(description='Costas-brein SDR simulatie')
    parser.add_argument('--p', type=int, default=701, help='Kolom-7 priem')
    parser.add_argument('--band', type=float, nargs=2, default=[500, 1500],
                        help='Frequentieband (MHz)')
    parser.add_argument('--sigma', type=float, default=0.995,
                        help='Conductiviteitsfactor grijze stof')
    parser.add_argument('--noise', type=float, default=0.001,
                        help='Ruisniveau')
    parser.add_argument('--list-primes', action='store_true',
                        help='Toon beschikbare kolom-7 priemen')
    args = parser.parse_args()
    
    print("╔═══════════════════════════════════════════════════════════╗")
    print("║     COSTAS-BREIN SDR KETEN SIMULATIE                     ║")
    print("║     9-kolom priemen → Welch-Costas → radar detectie     ║")
    print("╚═══════════════════════════════════════════════════════════╝")
    
    # ─── Beschikbare priemen ───
    col7 = kolom_7_priemen(50000)
    
    if args.list_primes:
        print(f"\nKolom-7 priemen tot 50.000: {len(col7)}")
        print(f"Eerste 30: {col7[:30]}")
        print(f"Laatste 10: {col7[-10:]}")
        print(f"\nGeschikt voor Costas (p > 10): {len([p for p in col7 if p > 10])}")
        return
    
    # ─── Kies priem ───
    p = args.p
    if p not in col7:
        # Vind dichtstbijzijnde
        p = min(col7, key=lambda x: abs(x - p))
        print(f"\n[*] p={args.p} is geen kolom-7 priem. Gebruik p={p}.")
    
    # ─── Genereer Costas ───
    print(f"\n{'─'*72}")
    print("STAP 1: Costas generator")
    print(f"{'─'*72}")
    
    costas = CostasGenerator(p, args.band)
    info = costas.info()
    
    print(f"  p        = {info['p']} (kolom {info['kolom']})")
    print(f"  α        = {info['alpha']}")
    print(f"  order N  = {info['order']}")
    print(f"  TB       = {info['tb_product']:,}")
    print(f"  PG       = {info['pg_db']} dB")
    print(f"  band     = {info['band'][0]:.0f}-{info['band'][1]:.0f} MHz")
    print(f"  tot tijd = {info['total_tijd_us']:.0f} μs")
    
    print(f"\n  Eerste 10 hops:")
    print(f"  {'hop':>4} {'f_idx':>6} {'f (MHz)':>10} {'t (μs)':>8}")
    for i in range(min(10, costas.order)):
        print(f"  {i:>4} {costas.freq_idx[i]:>6} {costas.freq_mhz[i]:>10.2f} {costas.tijd_s[i]*1e6:>8.1f}")
    
    # ─── Autocorrelatie ───
    acf = autocorrelatie(costas, 30)
    max_side = max(abs(acf[1:]))
    print(f"\n  Autocorrelatie max sidelobe: {max_side:.6f}")
    
    # ─── Kruiscorrelatie (vergelijking met andere kolom-7 priem) ───
    andere_priemen = [q for q in col7 if q != p and q > 20][:3]
    print(f"\n{'─'*72}")
    print("STAP 2: Kruiscorrelatie — orthogonaliteit")
    print(f"{'─'*72}")
    
    for q in andere_priemen:
        try:
            ca2 = CostasGenerator(q, args.band)
            cross = kruiscorrelatie(costas, ca2)
            print(f"  cross(p={p}, p={q}) = {cross:.6f}  {'→ ORTHOGONAAL' if abs(cross) < 0.3 else '→ gedeeltelijk'}")
        except:
            pass
    
    # ─── Reflectie simulatie ───
    print(f"\n{'─'*72}")
    print("STAP 3: Meerlaagse reflectie — hoofdmodel")
    print(f"{'─'*72}")
    
    print(f"\n{'Freq (MHz)':>12} {'R_rust':>10} {'R_actief':>10} {'ΔR':>12} {'ΔR/R (%)':>10}")
    print(f"{'─'*56}")
    for f_test_mhz in [300, 500, 800, 1000, 1500]:
        freq = f_test_mhz * 1e6
        R_base = hoofd_reflectie(freq, 1.0)
        R_act = hoofd_reflectie(freq, args.sigma)
        delta = R_act - R_base
        delta_pct = delta / R_base * 100
        print(f"{f_test_mhz:>10} MHz {R_base:>10.6f} {R_act:>10.6f} {delta:>+12.8f} {delta_pct:>+9.4f}%")
    
    # ─── Complete SDR simulatie ───
    print(f"\n{'─'*72}")
    print(f"STAP 4: SDR keten — Costas zender → hoofd → ontvanger")
    print(f"{'─'*72}")
    print(f"  Costas p={p}, band {args.band[0]:.0f}-{args.band[1]:.0f} MHz")
    print(f"  σ-factor grijze stof: {args.sigma}")
    print(f"  Ruisniveau: {args.noise}")
    
    fs = 20e6  # 20 MHz sample rate
    tx, rx, reflecties = sdr_keten_simulatie(costas, args.sigma, fs, args.noise)
    
    # Matched filter detectie
    corr = costas_matched_filter(rx, costas, fs)
    
    # Baseline (rust, σ=1.0)
    _, rx_rust, _ = sdr_keten_simulatie(costas, 1.0, fs, args.noise)
    corr_rust = costas_matched_filter(rx_rust, costas, fs)
    
    # Detectie index
    detectie = (corr - corr_rust) / (corr_rust + 1e-15) * 100
    
    print(f"\n  Matched filter resultaten:")
    print(f"    Correlatie (σ={args.sigma:.3f}): {corr:.6f}")
    print(f"    Correlatie (σ=1.0, rust): {corr_rust:.6f}")
    print(f"    Δcorr: {corr - corr_rust:+.6f} ({detectie:+.3f}%)")
    print(f"    SNR: {10*math.log10(np.var(np.abs(rx))/(np.var(np.abs(rx - tx*hoofd_reflectie(costas.freq_mhz[0]*1e6, 1.0)))+1e-15)):.1f} dB")
    
    # ─── Sigma sweep ───
    print(f"\n{'─'*72}")
    print("STAP 5: Sigma sweep — detectiegevoeligheid")
    print(f"{'─'*72}")
    
    print(f"\n{'Δσ (%)':>10} {'σ-factor':>10} {'ΔR/R (%)':>10} {'Correlatie':>12} {'Detectie Δ%':>12}")
    print(f"{'─'*56}")
    for sigma in [1.0, 0.999, 0.997, 0.995, 0.99, 0.98, 0.95, 0.90]:
        _, rx_s, refl = sdr_keten_simulatie(costas, sigma, fs, args.noise)
        corr_s = costas_matched_filter(rx_s, costas, fs)
        delta_corr = (corr_s - corr_rust) / (corr_rust + 1e-15) * 100
        avg_delta_r = np.mean([r[2] for r in refl]) if refl else 0
        avg_r_base = np.mean([r[0] for r in refl]) if refl else 1
        print(f"{(1-sigma)*100:>9.3f}% {sigma:>10.4f} {avg_delta_r/avg_r_base*100:>+10.4f}% {corr_s:>12.6f} {delta_corr:>+11.4f}%")
    
    # ─── Conclusie ───
    print(f"\n{'─'*72}")
    print("SAMENVATTING — systeemparameters")
    print(f"{'─'*72}")
    print(f"""
    Costas basis:  p={p} (kolom {costas.kolom}, α={costas.alpha})
    Order:         N={costas.order}
    TB product:    {costas.tb_product:,}
    Processing gain: {costas.pg_db:.1f} dB
    
    Frequentie:    {args.band[0]:.0f}-{args.band[1]:.0f} MHz
    Hops:          {costas.order}
    Hop tijd:      {costas.hop_tijd_s*1e6:.1f} μs
    Totale tijd:   {costas.order * costas.hop_tijd_s * 1e6:.0f} μs
    
    Detectie (σ={args.sigma}): correlatie Δ = {detectie:+.3f}%
    
    Kanaal-scheiding: {len(andere_priemen)+1} orthogonale kanalen (per kolom-7 priem)
    Maximale kanalen: {len(col7)} (alle kolom-7 priemen)
    
    ╔══ AANBEVOLEN HARDWARE ═══════════════════════════╗
    ║  SDR:      HackRF One (1-6000 MHz)               ║
    ║           of ADALM-Pluto (325-3800 MHz)          ║
    ║  Antenne:  Ultra-wideband 500-2000 MHz           ║
    ║  Sample rate: 20 MHz                             ║
    ║  Versterker: LNA 20-30 dB                        ║
    ╚══════════════════════════════════════════════════╝
    """)


if __name__ == "__main__":
    main()
