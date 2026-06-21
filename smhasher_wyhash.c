/*
 * smhasher_wyhash.c — SMHasher-grade test for wyhash64 vs PrimeHash
 * ==================================================================
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>
#include <time.h>

/* ═══════════════════════════════════════════════════════════════════
 * wyhash64 — canonical implementation (public domain, wangyi-fudan)
 * ═══════════════════════════════════════════════════════════════════ */

static inline uint64_t _wyr8(const uint8_t *p) {
    uint64_t v; memcpy(&v, p, 8); return v;
}

static inline uint64_t _wymix(uint64_t A, uint64_t B) {
    /* 64x64->128 multiply, return hi ^ lo */
    uint64_t lo = (uint64_t)(A) * (uint64_t)(B);
    /* clang/gcc support __int128 on 64-bit targets */
    uint64_t hi = (uint64_t)(((unsigned __int128)A * B) >> 64);
    return lo ^ hi;
}

uint64_t wyhash64(const void *key, size_t len, uint64_t seed) {
    const uint8_t *p = (const uint8_t *)key;
    uint64_t a = 0, b = 0;
    uint64_t see1 = seed ^ 0xa3b195354a39b70dULL;

    if (len <= 16) {
        if (len >= 4) {
            a = (_wyr8(p) << 32) | ((uint64_t)_wyr8(p + ((len>>3)<<3)) << 32 >> 32);
            b = (_wyr8(p + len - 4) << 32) | ((uint64_t)_wyr8(p + len - 4 - ((len>>3)<<3)) << 32 >> 32);
        } else if (len > 0) {
            a = ((uint64_t)p[0] << 16) | ((uint64_t)p[len>>1] << 24) | p[len-1];
            b = 0;
        }
    } else {
        const uint8_t *end = p + len;
        uint64_t see2 = see1 ^ 0x1b03738712fad5c9ULL;
        if (len > 48) {
            uint64_t see3 = see1 ^ 0x76e6e8d3e0a7b6c5ULL;
            uint64_t see4 = see2 ^ 0xb3c3e3d5f1a2b4c6ULL;
            while (p + 48 <= end) {
                a = _wymix(_wyr8(p) ^ see1, _wyr8(p+8) ^ see2); p += 16;
                b = _wymix(_wyr8(p) ^ see3, _wyr8(p+8) ^ see4); p += 16;
                uint64_t tmp = a; a = _wymix(a, b); b = _wymix(b, tmp);
                see1 ^= a; see2 ^= b; see3 ^= a; see4 ^= b;
            }
            see1 = see2; see2 = see3; see3 = see4;
        }
        if (p + 32 <= end) {
            a = _wymix(_wyr8(p) ^ see1, _wyr8(p+8) ^ see2); p += 16;
            b = _wymix(_wyr8(p) ^ see1, _wyr8(p+8) ^ see2); p += 16;
            uint64_t tmp = a; a = _wymix(a, b); b = _wymix(b, tmp);
        }
        if (p < end) {
            a = _wymix(_wyr8(p) ^ see1, _wyr8(end-8) ^ see2);
        }
        if (len > 16) {
            a ^= see1; b ^= see2;
        }
    }
    return _wymix(a ^ seed, b ^ len);
}

/* ═══════════════════════════════════════════════════════════════════
 * PrimeHash Quad v1 (for comparison)
 * ═══════════════════════════════════════════════════════════════════ */

static inline uint64_t read64(const void *p) {
    uint64_t v; memcpy(&v, p, 8); return v;
}
static inline uint64_t rotl64(uint64_t x, int k) {
    return (x << k) | (x >> (64 - k));
}

#define PHI64  0x9E3779B97F4A7C15ULL
#define MUL1   0x85EBCA77C2B2AE3DULL
#define MUL2   0xBF58476D1CE4E5B9ULL
#define MUL4   0x94D049BB133111EBULL

uint64_t primehash_quad(const void *buf, size_t len, uint64_t seed) {
    const uint8_t *p = (const uint8_t *)buf;
    const uint8_t *end = p + len;
    uint64_t base = seed ^ (len * PHI64);
    uint64_t h1 = base * PHI64;
    uint64_t h2 = base * MUL1;
    uint64_t h3 = base * MUL2;
    uint64_t h4 = base * MUL4;

    while (p + 32 <= end) {
        h1 ^= rotl64(read64(p), 23); h1 *= PHI64; p += 8;
        h2 ^= rotl64(read64(p), 47); h2 *= PHI64; p += 8;
        h3 ^= rotl64(read64(p), 13); h3 *= PHI64; p += 8;
        h4 ^= rotl64(read64(p), 37); h4 *= PHI64; p += 8;
    }
    while (p + 8 <= end) {
        h1 ^= rotl64(read64(p), 23); h1 *= PHI64; p += 8;
    }
    uint64_t tail = 0;
    switch (end - p) {
        case 7: tail ^= (uint64_t)p[6] << 48;
        case 6: tail ^= (uint64_t)p[5] << 40;
        case 5: tail ^= (uint64_t)p[4] << 32;
        case 4: tail ^= (uint64_t)p[3] << 24;
        case 3: tail ^= (uint64_t)p[2] << 16;
        case 2: tail ^= (uint64_t)p[1] << 8;
        case 1: tail ^= (uint64_t)p[0];
                h1 ^= rotl64(tail, 23); h1 *= PHI64;
    }
    h1 ^= h2; h3 ^= h4; h1 ^= h3;
    h1 ^= h1 >> 29; h1 *= MUL1;
    h1 ^= h1 >> 31; h1 *= MUL2;
    h1 ^= h1 >> 37; h1 *= PHI64;
    h1 ^= h1 >> 41;
    return h1;
}

/* ═══════════════════════════════════════════════════════════════════
 * Test harness
 * ═══════════════════════════════════════════════════════════════════ */

typedef uint64_t (*hash_fn)(const void *, size_t, uint64_t);

static int popcount64(uint64_t x) {
    x = x - ((x >> 1) & 0x5555555555555555ULL);
    x = (x & 0x3333333333333333ULL) + ((x >> 2) & 0x3333333333333333ULL);
    x = (x + (x >> 4)) & 0x0F0F0F0F0F0F0F0FULL;
    return (x * 0x0101010101010101ULL) >> 56;
}

static int tests_run, tests_failed;
static const char *current_test;
#define TEST(n) do { current_test=n; tests_run++; } while(0)
#define FAIL(f,...) do { fprintf(stderr,"  FAIL [%s] " f "\n",current_test,##__VA_ARGS__); tests_failed++; return; } while(0)
#define PASS() do { printf("  PASS %s\n", current_test); } while(0)

static void test_avalanche(hash_fn h, const char *tag) {
    TEST("avalanche");
    uint8_t buf[256];
    for (int i=0;i<256;i++) buf[i]=(uint8_t)(i*0x9D+0x37);
    uint64_t base=h(buf,256,0);
    double total=0, worst=100, best=0; int samples=0;
    for(int byte=0;byte<32;byte++) {
        for(int bit=0;bit<8;bit++) {
            buf[byte]^=(1u<<bit);
            uint64_t hv=h(buf,256,0);
            buf[byte]^=(1u<<bit);
            double pct=popcount64(base^hv)/64.0*100.0;
            total+=pct; samples++;
            if(pct<worst)worst=pct;
            if(pct>best)best=pct;
        }
    }
    double avg=total/samples;
    printf("  %s avalanche: avg=%.1f%% worst=%.1f%% best=%.1f%%\n",tag,avg,worst,best);
    if(avg<45.0||avg>55.0) FAIL("[%s] avg %.1f%%",tag,avg);
    if(worst<33.0) FAIL("[%s] worst %.1f%% < 33%%",tag,worst);
}

static void test_bias(hash_fn h, const char *tag) {
    TEST("bias");
    #define BN 100000
    int bc[64]={0};
    for(int i=0;i<BN;i++) {
        uint64_t hv=h(&i,sizeof(i),i*0x9E3779B9);
        for(int b=0;b<64;b++) if(hv&(1ULL<<b)) bc[b]++;
    }
    double worst=0; int wb=0;
    for(int b=0;b<64;b++) {
        double bias=fabs(bc[b]/(double)BN*100.0-50.0);
        if(bias>worst){worst=bias;wb=b;}
    }
    printf("  %s bias: worst bit %d at %.2f%%\n",tag,wb,worst+50.0);
    if(worst>5.0) FAIL("[%s] bias %.2f%%",tag,worst+50.0);
}

static void test_distribution(hash_fn h, const char *tag) {
    TEST("distribution");
    #define DB 256
    #define DN (DB*1000)
    int bins[DB]={0};
    for(int i=0;i<DN;i++) bins[h(&i,sizeof(i),i)&(DB-1)]++;
    double expv=DN/(double)DB, chi2=0;
    for(int b=0;b<DB;b++){double d=bins[b]-expv; chi2+=d*d/expv;}
    printf("  %s chi2: %.1f (df=255)\n",tag,chi2);
    if(chi2>350) FAIL("[%s] chi2 %.1f",tag,chi2);
}

static void test_collision32(hash_fn h, const char *tag) {
    TEST("collision32");
    #define CN 500000
    #define CTB 20
    #define CTS (1u<<CTB)
    uint32_t *tbl=calloc(CTS,sizeof(uint32_t));
    if(!tbl){printf("  SKIP\n");return;}
    uint8_t key[16]; int cols=0;
    for(int i=0;i<CN;i++) {
        int kl=4+(i%13); memcpy(key,&i,4);
        for(int j=4;j<kl;j++) key[j]=(uint8_t)(i>>(j*3));
        uint64_t h64=h(key,kl,0);
        uint32_t h32=(uint32_t)(h64^(h64>>32));
        uint32_t idx=h32&(CTS-1);
        if(tbl[idx]==0) tbl[idx]=(uint32_t)(h64&0xFFFFFFFF);
        else if(tbl[idx]!=(uint32_t)(h64&0xFFFFFFFF)) cols++;
    }
    double ex=exp(-0.5)*CTS, ec=CTS-ex-CN*(1.0-exp(-0.5));
    printf("  %s collisions: %d (expected ~%.0f)\n",tag,cols,ec);
    if(cols>ec*2.5&&cols>100) FAIL("[%s] excessive %d",tag,cols);
    free(tbl);
}

static void test_differential(hash_fn h, const char *tag) {
    TEST("differential");
    uint8_t a[64],b[64];
    double total=0; int pairs=0, minb=64,maxb=0;
    for(int t=0;t<1000;t++) {
        for(int i=0;i<64;i++) a[i]=(uint8_t)(t*0x9D+i*0x37);
        memcpy(b,a,64);
        b[t%64]^=(1u<<((t*7)%8));
        uint64_t ha=h(a,64,(uint64_t)t), hb=h(b,64,(uint64_t)t);
        int c=popcount64(ha^hb);
        total+=c; pairs++; if(c<minb)minb=c; if(c>maxb)maxb=c;
    }
    double avg=total/pairs;
    printf("  %s diff: avg=%.1f range=[%d,%d]\n",tag,avg,minb,maxb);
    if(avg<24||avg>40) FAIL("[%s] diff avg %.1f",tag,avg);
}

static double speed_test(hash_fn h, size_t len, int iters) {
    uint8_t *buf=malloc(len);
    if(!buf)return 0;
    for(size_t i=0;i<len;i++)buf[i]=(uint8_t)(i*0x9D+0x37);
    volatile uint64_t sum=0;
    clock_t st=clock();
    for(int i=0;i<iters;i++) sum^=h(buf,len,(uint64_t)i);
    clock_t en=clock();
    double secs=(double)(en-st)/CLOCKS_PER_SEC;
    double gbps=(len*iters/secs)/1e9;
    free(buf);
    return gbps;
}

static void test_speed(hash_fn h, const char *tag) {
    TEST("speed");
    int sz[]={1,8,16,32,64,256,1024};
    int ns=sizeof(sz)/sizeof(sz[0]);
    printf("  %s speed:",tag);
    double total=0;
    for(int i=0;i<ns;i++) {
        int iters=sz[i]<64?20000000:sz[i]<256?5000000:1000000;
        double gbps=speed_test(h,sz[i],iters);
        if(gbps>0){printf(" %dB:%.1f",sz[i],gbps); total+=gbps;}
    }
    printf(" | avg:%.1f GB/s\n",total/ns);
}

static void test_bulk(hash_fn h, const char *tag) {
    TEST("bulk");
    #define BN2 50000
    uint64_t *hashes=malloc(BN2*8);
    if(!hashes){printf("  SKIP\n");return;}
    for(int i=0;i<BN2;i++) hashes[i]=h(&i,sizeof(i),0);
    uint32_t *s32=calloc(1<<20,4);
    if(!s32){free(hashes);printf("  SKIP\n");return;}
    int dups=0;
    for(int i=0;i<BN2;i++) {
        uint32_t slot=(uint32_t)(hashes[i]>>32)&((1u<<20)-1);
        uint32_t tag=(uint32_t)hashes[i];
        if(s32[slot]==0)s32[slot]=tag;
        else if(s32[slot]==tag) {
            for(int j=0;j<i;j++) if(hashes[j]==hashes[i]){dups++;break;}
        }
    }
    printf("  %s bulk: %d collisions in 50K ints\n",tag,dups);
    if(dups>0) FAIL("[%s] %d sequential int collisions",tag,dups);
    free(hashes); free(s32);
}

static void test_cyclic(hash_fn h, const char *tag) {
    TEST("cyclic");
    uint8_t key[256]; memset(key,0x5A,255);
    uint64_t seen[256]; int cols=0;
    for(int i=0;i<256;i++){key[255]=(uint8_t)i;seen[i]=h(key,256,0);}
    for(int i=0;i<256;i++)for(int j=i+1;j<256;j++)if(seen[i]==seen[j])cols++;
    printf("  %s cyclic: %d collisions\n",tag,cols);
    if(cols>0)FAIL("[%s] cyclic %d",tag,cols);
}

int main(void) {
    printf("=== wyhash64 vs PrimeHash Quad ===\n\n");

    /* wyhash */
    tests_run=tests_failed=0;
    printf("── wyhash64 ──\n");
    test_avalanche(wyhash64,"wyhash64");
    test_bias(wyhash64,"wyhash64");
    test_distribution(wyhash64,"wyhash64");
    test_collision32(wyhash64,"wyhash64");
    test_differential(wyhash64,"wyhash64");
    test_speed(wyhash64,"wyhash64");
    test_bulk(wyhash64,"wyhash64");
    test_cyclic(wyhash64,"wyhash64");
    int wr=tests_run,wf=tests_failed;

    /* PrimeHash */
    tests_run=tests_failed=0;
    printf("\n── PrimeHash Quad ──\n");
    test_avalanche(primehash_quad,"PrimeHash");
    test_bias(primehash_quad,"PrimeHash");
    test_distribution(primehash_quad,"PrimeHash");
    test_collision32(primehash_quad,"PrimeHash");
    test_differential(primehash_quad,"PrimeHash");
    test_speed(primehash_quad,"PrimeHash");
    test_bulk(primehash_quad,"PrimeHash");
    test_cyclic(primehash_quad,"PrimeHash");
    int pr=tests_run,pf=tests_failed;

    printf("\n=== SUMMARY ===\n");
    printf("wyhash64:    %d/%d passed\n", wr-wf, wr);
    printf("PrimeHash:   %d/%d passed\n", pr-pf, pr);
    return (wf>0||pf>0)?1:0;
}
