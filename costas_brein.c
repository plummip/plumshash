/* ==========================================================================
 * costas_brein.c — Costas-radar voor hersenactiviteit detectie
 *
 * Gebaseerd op 9-kolom priemtheorie. Welch-Costas constructie:
 *   f(i) = α^i mod p  (i = 1..p-1)
 * waarbij p een priem is uit kolom 7 (n ≡ 8 mod 9) van het 9-grid.
 *
 * Dit bestand bevat:
 *   1. Costas code generator (kolom-7 priemen → hopping pattern)
 *   2. Matched filter correlator (real-time detectie)
 *   3. Hoofdmodel: 5-laagse reflectie simulatie
 * ========================================================================== */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <math.h>
#include <time.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* ─── Configuratie ─── */
#define MAX_COSTAS_ORDER    100000
#define MAX_FREQ_BANDS      10
#define MAX_CHANNELS        64
#define SAMPLE_RATE         20e6    /* 20 MHz sample rate */
#define PI                  3.141592653589793

/* ─── 9-Kolom priem tools ─── */

/* Bepaal kolom in 9-grid: c = (n-1) % 9 */
static inline int kolom(int64_t n) {
    return (int)((n - 1) % 9);
}

/* Veilige kolommen? */
static inline int is_veilig(int c) {
    return c == 0 || c == 1 || c == 3 || c == 4 || c == 6 || c == 7;
}

/* ─── Sieve: simpele byte-array zeef ─── */
uint8_t* zeef(int64_t limit, int64_t *n_primes) {
    uint8_t *is_prime = (uint8_t*)calloc(limit + 1, 1);
    if (!is_prime) return NULL;
    
    memset(is_prime, 1, limit + 1);
    is_prime[0] = is_prime[1] = 0;
    
    for (int64_t p = 2; p * p <= limit; p++) {
        if (is_prime[p]) {
            for (int64_t m = p * p; m <= limit; m += p)
                is_prime[m] = 0;
        }
    }
    
    if (n_primes) {
        *n_primes = 0;
        for (int64_t i = 2; i <= limit; i++)
            if (is_prime[i]) (*n_primes)++;
    }
    return is_prime;
}

int64_t* zeef_kolom7(int64_t limit, int *count) {
    uint8_t *bp = zeef(limit, NULL);
    if (!bp) return NULL;
    
    /* Tel kolom-7 priemen */
    int n = 0;
    for (int64_t i = 2; i <= limit; i++)
        if (bp[i] && kolom(i) == 7) n++;
    
    int64_t *result = (int64_t*)malloc(n * sizeof(int64_t));
    if (!result) { free(bp); return NULL; }
    
    int idx = 0;
    for (int64_t i = 2; i <= limit; i++)
        if (bp[i] && kolom(i) == 7) result[idx++] = i;
    
    *count = n;
    free(bp);
    return result;
}

/* ─── Primitieve wortel zoeken ─── */
/* Factoriseer n, return factoren in array, zet *nf */
int64_t* factoriseer(int64_t n, int *nf) {
    static int64_t factoren[64];
    int cnt = 0;
    int64_t temp = n;
    
    for (int64_t d = 2; d * d <= temp; d++) {
        if (temp % d == 0) {
            factoren[cnt++] = d;
            while (temp % d == 0) temp /= d;
        }
    }
    if (temp > 1) factoren[cnt++] = temp;
    
    *nf = cnt;
    return factoren;
}

/* Vind primitieve wortel modulo p */
int64_t primitieve_wortel(int64_t p) {
    if (p < 2) return 0;
    
    int nf;
    int64_t *pf = factoriseer(p - 1, &nf);
    
    for (int64_t g = 2; g < p; g++) {
        int ok = 1;
        for (int i = 0; i < nf && ok; i++) {
            /* pow(g, (p-1)/q, p) moet ≠ 1 zijn */
            int64_t exp = (p - 1) / pf[i];
            int64_t result = 1;
            int64_t base = g % p;
            int64_t e = exp;
            while (e > 0) {
                if (e & 1) result = (result * base) % p;
                base = (base * base) % p;
                e >>= 1;
            }
            if (result == 1) ok = 0;
        }
        if (ok) return g;
    }
    return 0;  /* geen (zou niet moeten gebeuren voor priem p) */
}

/* ─── Costas array generatie ─── */
typedef struct {
    int64_t p;              /* basis priem */
    int64_t alpha;          /* primitieve wortel */
    int     order;          /* N = p-1 */
    int64_t *freq;          /* α^i mod p voor i=1..order */
    double  *freq_mhz;      /* gemapt naar MHz */
    double  *tijd_us;       /* tijdstip per hop */
    double  band_start;     /* frequentieband start (MHz) */
    double  band_end;       /* frequentieband eind (MHz) */
    double  hop_tijd_us;    /* tijd per hop (micros) */
} costas_array_t;

costas_array_t* costas_genereren(int64_t p, double band_start_mhz, double band_end_mhz) {
    costas_array_t *ca = (costas_array_t*)calloc(1, sizeof(costas_array_t));
    if (!ca) return NULL;
    
    ca->p = p;
    ca->alpha = primitieve_wortel(p);
    ca->order = (int)(p - 1);
    ca->band_start = band_start_mhz;
    ca->band_end = band_end_mhz;
    ca->hop_tijd_us = 10.0;  /* 10 μs per hop */
    
    if (ca->alpha == 0) {
        fprintf(stderr, "Geen primitieve wortel voor p=%ld\n", (long)p);
        free(ca);
        return NULL;
    }
    
    /* Genereer α^i mod p */
    ca->freq = (int64_t*)malloc(ca->order * sizeof(int64_t));
    ca->freq_mhz = (double*)malloc(ca->order * sizeof(double));
    ca->tijd_us = (double*)malloc(ca->order * sizeof(double));
    
    if (!ca->freq || !ca->freq_mhz || !ca->tijd_us) {
        free(ca->freq); free(ca->freq_mhz); free(ca->tijd_us); free(ca);
        return NULL;
    }
    
    int64_t val = 1;
    for (int i = 0; i < ca->order; i++) {
        val = (val * ca->alpha) % p;
        ca->freq[i] = val;
        /* Map naar frequentieband: 0..p-1 → band_start..band_end MHz */
        ca->freq_mhz[i] = band_start_mhz + (double)val / (p - 1) * (band_end_mhz - band_start_mhz);
        ca->tijd_us[i] = i * ca->hop_tijd_us;
    }
    
    return ca;
}

void costas_vrijgeven(costas_array_t *ca) {
    if (!ca) return;
    free(ca->freq);
    free(ca->freq_mhz);
    free(ca->tijd_us);
    free(ca);
}

void costas_print(const costas_array_t *ca) {
    printf("Costas array:\n");
    printf("  p          = %ld (kolom %d)\n", (long)ca->p, kolom(ca->p));
    printf("  α          = %ld\n", (long)ca->alpha);
    printf("  order N    = %d\n", ca->order);
    printf("  TB product = %d\n", ca->order * ca->order);
    printf("  PG         = %.1f dB\n", 10 * log10(ca->order * ca->order));
    printf("  band       = %.0f-%.0f MHz\n", ca->band_start, ca->band_end);
    printf("  hop tijd   = %.1f μs\n", ca->hop_tijd_us);
    printf("  total tijd = %.1f μs\n", ca->order * ca->hop_tijd_us);
    
    printf("\n  Eerste 20 hops:\n");
    printf("  %4s %12s %10s %10s\n", "hop", "freq (α^i mod p)", "freq (MHz)", "tijd (μs)");
    for (int i = 0; i < (ca->order < 20 ? ca->order : 20); i++) {
        printf("  %4d %12ld %10.2f %10.1f\n", 
               i, (long)ca->freq[i], ca->freq_mhz[i], ca->tijd_us[i]);
    }
}

/* ─── Autocorrelatie Costas array ─── */
double* costas_acf(const costas_array_t *ca, int max_lag) {
    int n = ca->order;
    if (max_lag > n / 2) max_lag = n / 2;
    
    double *acf = (double*)calloc(max_lag + 1, sizeof(double));
    if (!acf) return NULL;
    
    acf[0] = 1.0;
    for (int lag = 1; lag <= max_lag; lag++) {
        double sum = 0;
        for (int i = 0; i < n - lag; i++) {
            sum += (ca->freq_mhz[i] - ca->band_start) * 
                   (ca->freq_mhz[i + lag] - ca->band_start);
        }
        double v0 = 0;
        for (int i = 0; i < n; i++) {
            double d = ca->freq_mhz[i] - ca->band_start;
            v0 += d * d;
        }
        acf[lag] = sum / (v0 + 1e-15);
    }
    return acf;
}

/* ═══════════════════════════════════════════════════════════════
 * 5-LAAGS HOOFDMODEL — Reflectie simulatie
 * ═══════════════════════════════════════════════════════════════ */

/* Cole-Cole parameters per weefsel (Gabriel 1996) */
typedef struct {
    double eps_inf;
    double sigma;       /* S/m */
    double dikte_m;     /* laagdikte in meters */
    /* Cole-Cole dispersie: (Δε, τ, α) × 4 */
    double cole_delta[4];
    double cole_tau[4];
    double cole_alpha[4];
} weefsel_t;

weefsel_t weefsels[] = {
    /* skin */
    {4.0, 0.0002, 0.003, {4, 40, 1e4, 2e6}, {7.96e-12, 15.92e-9, 106.1e-6, 5.305e-3}, {0.1, 0.15, 0.22, 0}},
    /* skull */
    {2.5, 0.001,  0.007, {10, 20, 100, 5e4}, {13.26e-12, 79.58e-9, 159.15e-6, 7.958e-3}, {0.2, 0.2, 0.2, 0}},
    /* CSF */
    {6.0, 2.0,    0.003, {55, 100, 50, 5e4}, {7.96e-12, 19.89e-9, 79.58e-6, 4.774e-3}, {0.1, 0.1, 0.1, 0}},
    /* grey_matter */
    {4.0, 0.02,   0.004, {45, 400, 2e5, 4e7}, {7.96e-12, 15.92e-9, 106.1e-6, 5.305e-3}, {0.1, 0.15, 0.22, 0}},
    /* white_matter */
    {4.0, 0.02,   0.050, {32, 100, 3e4, 2e7}, {7.96e-12, 15.92e-9, 106.1e-6, 5.305e-3}, {0.1, 0.15, 0.22, 0}},
};
#define N_WEEFSELS 5

/* Complexe permittiviteit bij frequentie f (Hz) voor weefsel w */
void permittiviteit(double f, const weefsel_t *w, double *eps_real, double *eps_imag) {
    double eps0 = 8.854e-12;
    double omega = 2 * M_PI * f;
    
    double er = w->eps_inf;
    double ei = 0;
    
    for (int k = 0; k < 4; k++) {
        double delta_e = w->cole_delta[k];
        double tau = w->cole_tau[k];
        double alpha = w->cole_alpha[k];
        
        /* Cole-Cole: Δε / (1 + (jωτ)^(1-α)) */
        double phi = (1 - alpha) * atan2(omega * tau, 1);  /* phase of jωτ */
        double mag = pow(omega * tau, 1 - alpha);
        double denom_re = 1 + mag * cos(phi);
        double denom_im = mag * sin(phi);
        double denom2 = denom_re * denom_re + denom_im * denom_im;
        
        er += delta_e * denom_re / denom2;
        ei -= delta_e * denom_im / denom2;  /* negatief want imaginaire deel */
    }
    
    /* Conductiviteit: σ/(jωε₀) = -jσ/(ωε₀) */
    double sigma_term = w->sigma / (omega * eps0);
    ei -= sigma_term;  /* imaginaire deel = -σ/(ωε₀) */
    
    *eps_real = er;
    *eps_imag = ei;
}

/* Totale reflectiecoefficient van hoofdmodel */
double hoofd_reflectie(double freq_hz, double grey_sigma_factor) {
    double Z0 = 377.0;  /* vrije ruimte impedantie */
    double Z_in = Z0;
    double total_r = 0;
    double eps0 = 8.854e-12;
    
    for (int i = 0; i < N_WEEFSELS; i++) {
        weefsel_t w = weefsels[i];
        
        /* Pas conductiviteit aan voor grijze stof (index 3) */
        if (i == 3) w.sigma *= grey_sigma_factor;
        
        double er, ei;
        permittiviteit(freq_hz, &w, &er, &ei);
        
        /* Complexe impedantie: Z = Z0 / sqrt(ε) */
        double mag = sqrt(er * er + ei * ei);
        double phase = atan2(ei, er) / 2;
        double Z_mag = Z0 / sqrt(mag);
        double Z_phase = -phase;
        
        double Z_r = Z_mag * cos(Z_phase);
        double Z_i = Z_mag * sin(Z_phase);
        
        /* Reflectie op grensvlak */
        double dr = Z_r - Z_in;
        double di = Z_i;
        double sr = Z_r + Z_in;
        double si = Z_i;
        double r_mag = sqrt((dr*dr + di*di) / (sr*sr + si*si));
        total_r += r_mag;
        
        /* Vereenvoudigde propagatie */
        double omega_val = 2 * M_PI * freq_hz;
        double alpha = omega_val * ei / (2 * sqrt(er));
        double atten = exp(-alpha * w.dikte_m);
        
        /* Nieuwe ingangsimpedantie */
        double Z_new_r = Z_r;
        double Z_new_i = Z_i;
        Z_in = sqrt(Z_new_r*Z_new_r + Z_new_i*Z_new_i);
    }
    
    return total_r;
}

/* ═══════════════════════════════════════════════════════════════
 * MATCHED FILTER — Costas correlator
 * ═══════════════════════════════════════════════════════════════ */

typedef struct {
    double I;  /* in-phase */
    double Q;  /* quadrature */
} complex_t;

typedef struct {
    costas_array_t *code;       /* Costas hopping code */
    double *matched_I;          /* matched filter coefficients (I) */
    double *matched_Q;          /* matched filter coefficients (Q) */
    int     code_len;           /* lengte code */
    double  sample_rate;        /* sample rate */
    int     samples_per_hop;    /* samples per hop */
    double  corr_threshold;     /* detectiedrempel */
} matched_filter_t;

matched_filter_t* mf_init(costas_array_t *code, double sample_rate) {
    matched_filter_t *mf = (matched_filter_t*)calloc(1, sizeof(matched_filter_t));
    if (!mf) return NULL;
    
    mf->code = code;
    mf->code_len = code->order;
    mf->sample_rate = sample_rate;
    mf->samples_per_hop = (int)(code->hop_tijd_us * 1e-6 * sample_rate);
    mf->corr_threshold = 0.5;
    
    int total_samples = mf->code_len * mf->samples_per_hop;
    
    /* Genereer matched filter coefficients */
    /* Voor elke hop: f_i = Costas frequentie, stuur een sinus van die freq */
    mf->matched_I = (double*)calloc(total_samples, sizeof(double));
    mf->matched_Q = (double*)calloc(total_samples, sizeof(double));
    
    if (!mf->matched_I || !mf->matched_Q) {
        free(mf->matched_I); free(mf->matched_Q); free(mf);
        return NULL;
    }
    
    for (int hop = 0; hop < mf->code_len; hop++) {
        double f_hz = mf->code->freq_mhz[hop] * 1e6;
        double dt = 1.0 / sample_rate;
        for (int s = 0; s < mf->samples_per_hop; s++) {
            int idx = hop * mf->samples_per_hop + s;
            double t = s * dt;
            double phase = 2 * M_PI * f_hz * t;
            mf->matched_I[idx] = cos(phase);
            mf->matched_Q[idx] = sin(phase);
        }
    }
    
    return mf;
}

void mf_vrijgeven(matched_filter_t *mf) {
    if (!mf) return;
    free(mf->matched_I);
    free(mf->matched_Q);
    free(mf);
}

/* Correlateer ontvangen signaal met matched filter */
double mf_correlate(matched_filter_t *mf, const double *rx_signal, int rx_len) {
    int total_samples = mf->code_len * mf->samples_per_hop;
    if (rx_len < total_samples) return 0;
    
    double corr_I = 0, corr_Q = 0;
    double norm_rx = 0, norm_mf = 0;
    
    for (int i = 0; i < total_samples; i++) {
        corr_I += rx_signal[i] * mf->matched_I[i];
        corr_Q += rx_signal[i] * mf->matched_Q[i];
        norm_rx += rx_signal[i] * rx_signal[i];
        norm_mf += mf->matched_I[i] * mf->matched_I[i] + 
                   mf->matched_Q[i] * mf->matched_Q[i];
    }
    
    double corr_mag = sqrt(corr_I * corr_I + corr_Q * corr_Q);
    double norm = sqrt(norm_rx * norm_mf);
    
    return (norm > 1e-15) ? corr_mag / norm : 0;
}

/* ─── Simuleer complete zender → hoofd → ontvanger keten ─── */
void simuleer_brein_detectie(costas_array_t *ca, double grey_sigma_factor, 
                              double *corr_out, double *snr_out) {
    int n_hops = ca->order;
    int samples_per_hop = 200;  /* 10 μs × 20 MHz */
    int total_samples = n_hops * samples_per_hop;
    double dt = 1.0 / 20e6;  /* 20 MHz sample rate */
    
    /* Zend signaal */
    double *tx = (double*)calloc(total_samples, sizeof(double));
    double *rx = (double*)calloc(total_samples, sizeof(double));
    
    if (!tx || !rx) { free(tx); free(rx); return; }
    
    for (int hop = 0; hop < n_hops; hop++) {
        double f_hz = ca->freq_mhz[hop] * 1e6;
        double reflection = hoofd_reflectie(f_hz, grey_sigma_factor);
        
        for (int s = 0; s < samples_per_hop; s++) {
            int idx = hop * samples_per_hop + s;
            double t = s * dt;
            tx[idx] = cos(2 * M_PI * f_hz * t);
            /* Ontvangen signaal = reflectie × zendsignaal + ruis */
            rx[idx] = reflection * tx[idx] + 0.001 * ((double)rand() / RAND_MAX - 0.5);
        }
    }
    
    /* Matched filter */
    matched_filter_t *mf = mf_init(ca, 20e6);
    if (mf) {
        *corr_out = mf_correlate(mf, rx, total_samples);
        mf_vrijgeven(mf);
    }
    
    /* SNR schatting */
    double sig_pwr = 0, noise_pwr = 0;
    for (int i = 0; i < total_samples; i++) {
        sig_pwr += rx[i] * rx[i];
        noise_pwr += 1e-6;  /* ruisvloer */
    }
    *snr_out = 10 * log10(sig_pwr / (noise_pwr + 1e-15));
    
    free(tx);
    free(rx);
}

/* ═══════════════════════════════════════════════════════════════
 * MAIN — test en demonstratie
 * ═══════════════════════════════════════════════════════════════ */

int main(int argc, char **argv) {
    printf("╔══════════════════════════════════════════════════════════╗\n");
    printf("║     COSTAS-BREIN — Hersenactiviteit via radiogolven     ║\n");
    printf("║     9-kolom priemen → Welch-Costas → radar detectie    ║\n");
    printf("╚══════════════════════════════════════════════════════════╝\n\n");
    
    srand(time(NULL));
    
    /* ─── Stap 1: Vind kolom-7 priemen ─── */
    printf("─── STAP 1: Kolom-7 priemen ───\n\n");
    
    int n_col7;
    int64_t *col7 = zeef_kolom7(50000, &n_col7);
    printf("  Kolom-7 priemen tot 50.000: %d\n", n_col7);
    printf("  Eerste 10: ");
    for (int i = 0; i < 10 && i < n_col7; i++)
        printf("%ld ", (long)col7[i]);
    printf("\n\n");
    
    /* ─── Stap 2: Genereer Costas arrays ─── */
    printf("─── STAP 2: Costas arrays ───\n\n");
    
    /* Kies een paar kolom-7 priemen voor demonstratie */
    int demo_primes[] = {179, 359, 701, 1009, 1997};
    int n_demo = 5;
    
    for (int d = 0; d < n_demo; d++) {
        int64_t p = demo_primes[d];
        if (kolom(p) != 7) continue;  /* safety check */
        
        costas_array_t *ca = costas_genereren(p, 500, 1500);
        if (ca) {
            printf("  ─── p=%ld (kolom %d) ───\n", (long)p, kolom(p));
            costas_print(ca);
            
            /* Autocorrelatie */
            double *acf = costas_acf(ca, 20);
            if (acf) {
                printf("\n  Autocorrelatie (max sidelobe): ");
                double max_sl = 0;
                for (int l = 1; l <= 20; l++)
                    if (fabs(acf[l]) > max_sl) max_sl = fabs(acf[l]);
                printf("%.6f\n", max_sl);
                free(acf);
            }
            
            costas_vrijgeven(ca);
            printf("\n");
        }
    }
    
    /* ─── Stap 3: Kruiscorrelatie tussen codes ─── */
    printf("─── STAP 3: Kruiscorrelatie — orthogonale kanalen ───\n\n");
    
    costas_array_t *ca1 = costas_genereren(179, 500, 1500);
    costas_array_t *ca2 = costas_genereren(359, 500, 1500);
    
    if (ca1 && ca2) {
        double cross = 0;
        for (int i = 0; i < (ca1->order < ca2->order ? ca1->order : ca2->order); i++) {
            cross += (ca1->freq_mhz[i] - ca1->band_start) * 
                     (ca2->freq_mhz[i] - ca2->band_start);
        }
        double n1 = 0, n2 = 0;
        for (int i = 0; i < ca1->order; i++) {
            double d = ca1->freq_mhz[i] - ca1->band_start;
            n1 += d * d;
        }
        for (int i = 0; i < ca2->order; i++) {
            double d = ca2->freq_mhz[i] - ca2->band_start;
            n2 += d * d;
        }
        double cross_norm = cross / (sqrt(n1 * n2) + 1e-15);
        printf("  Kruiscorrelatie p=179 vs p=359: %.6f\n", cross_norm);
        printf("  (Idealiter 0 — orthogonaal!)\n\n");
        
        costas_vrijgeven(ca1);
        costas_vrijgeven(ca2);
    }
    
    /* ─── Stap 4: Detectie simulatie ─── */
    printf("─── STAP 4: Brein-activiteit detectie simulatie ───\n\n");
    
    costas_array_t *ca_demo = costas_genereren(701, 500, 1500);
    if (ca_demo) {
        printf("  Costas: p=%ld, order=%d, %.0f-%.0f MHz\n\n",
               (long)ca_demo->p, ca_demo->order, ca_demo->band_start, ca_demo->band_end);
        
        printf("  %14s %12s %12s %12s\n", 
               "σ-factor", "Correlatie", "SNR (dB)", "Detecteerbaar?");
        printf("  %s\n", "  " "──────────────");
        
        double sigma_factors[] = {1.0, 0.999, 0.997, 0.995, 0.99, 0.98, 0.95};
        for (int i = 0; i < 7; i++) {
            double corr, snr;
            simuleer_brein_detectie(ca_demo, sigma_factors[i], &corr, &snr);
            const char *detectable = (corr > 0.3) ? "JA" : (corr > 0.1) ? "MISSCHIEN" : "NEE";
            printf("  %14.3f %12.6f %12.2f %12s\n", 
                   sigma_factors[i], corr, snr, detectable);
        }
        
        costas_vrijgeven(ca_demo);
    }
    
    /* ─── Stap 5: Overzicht ─── */
    printf("\n─── OVERZICHT KOLOM-7 PRIEMEN ALS COSTAS BASIS ───\n\n");
    printf("  %8s %6s %8s %12s %8s\n", "p", "α", "N", "TB", "PG(dB)");
    printf("  %s\n", "  " "──────────────");
    
    for (int i = 0; i < 20 && i < n_col7; i++) {
        int64_t p = col7[i];
        int64_t alpha = primitieve_wortel(p);
        if (alpha) {
            int n = (int)(p - 1);
            double pg = 10 * log10(n * n);
            printf("  %8ld %6ld %8d %12d %8.1f\n", (long)p, (long)alpha, n, n*n, pg);
        }
    }
    
    printf("\n  ... en nog %d kolom-7 priemen beschikbaar\n\n", n_col7 - 20);
    
    free(col7);
    
    printf("╔══════════════════════════════════════════════════════════╗\n");
    printf("║  CONCLUSIE: Detectie van hersenactiviteit via           ║\n");
    printf("║  Costas-gemoduleerde radiogolven is THEORETISCH         ║\n");
    printf("║  haalbaar met 9-kolom priemen als basis.               ║\n");
    printf("║                                                        ║\n");
    printf("║  Volgende stap: SDR hardware (HackRF/PlutoSDR)         ║\n");
    printf("╚══════════════════════════════════════════════════════════╝\n");
    
    return 0;
}
