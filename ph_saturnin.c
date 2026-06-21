/* ph_saturnin.c — Saturnin-inspired hash using GF multiply + S-box mixing */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#include <math.h>

static inline uint64_t rot(uint64_t x,int k){return(x<<k)|(x>>(64-k));}
#define PHI 0x9E3779B97F4A7C15ULL

/* GF(2^64) multiplication by PHI: shift+XOR based on PHI's bit pattern */
static inline uint64_t gf_mul_phi(uint64_t x) {
    /* PHI = 0x9E3779B97F4A7C15 = binary: ... */
    /* Compute x * PHI in GF(2^64) using shift-and-XOR */
    /* The exact shifts depend on PHI's polynomial representation */
    /* For simplicity, use precomputed shifts from PHI's bits */
    uint64_t r = 0;
    /* Bit 0 */ r ^= x;
    /* Bit 2 */ r ^= x << 2;
    /* Bit 4 */ r ^= x << 4;
    /* Bit 7 */ r ^= x << 7;
    /* Bit 8 */ r ^= x << 8;
    /* Bit 9 */ r ^= x << 9;
    /* Bit 10 */ r ^= x << 10;
    /* Bit 11 */ r ^= x << 11;
    /* Bit 12 */ r ^= x << 12;
    /* Bit 13 */ r ^= x << 13;
    /* Bit 14 */ r ^= x << 14;
    /* Bit 16 */ r ^= x << 16; /* ... continuing would be many */
    /* This approach is too verbose. Better: use the generic gf_mul */
    return r;
}

/* Generic GF(2^64) multiply: shift-and-XOR, ~64 iterations worst case */
static inline uint64_t gf_mul(uint64_t a, uint64_t b) {
    uint64_t r = 0;
    while (b) {
        if (b & 1) r ^= a;
        b >>= 1;
        a <<= 1;
    }
    return r;
}

/* Saturnin S-box sigma_0 on 4×16-bit lanes packed as 64-bit */
/* Uses only AND, OR, XOR — all 1-cycle operations */
static inline void sbox_64(uint64_t *a, uint64_t *b, uint64_t *c, uint64_t *d) {
    /* sigma_0: operates on 4 independent 16-bit lanes packed in 64-bit words */
    /* a ^= b & c;  b ^= a | d;  d ^= b | c;  c ^= b & d;  b ^= a | c;  a ^= b | d */
    /* Then rotate: a,b,c,d -> b,c,d,a */
    uint64_t ta = *a, tb = *b, tc = *c, td = *d;
    ta ^= tb & tc;
    tb ^= ta | td;
    td ^= tb | tc;
    tc ^= tb & td;
    tb ^= ta | tc;
    ta ^= tb | td;
    /* Rotate output assignment: b,c,d,a */
    *a = tb;
    *b = tc;
    *c = td;
    *d = ta;
}

/* v_sat: Saturnin-inspired hash — GF multiply + S-box in hot loop */
uint64_t v_sat(const void *buf, size_t len, uint64_t seed) {
    const uint8_t *p = (const uint8_t *)buf;
    const uint8_t *e = p + len;

    uint64_t ba = seed ^ (len * PHI);
    uint64_t h1 = ba;
    uint64_t h2 = ba ^ PHI;
    uint64_t h3 = ba ^ 0x85EBCA77C2B2AE3DULL;
    uint64_t h4 = ba ^ 0xBF58476D1CE4E5B9ULL;

    while (p + 32 <= e) {
        /* Load 4×8 bytes */
        uint64_t v1, v2, v3, v4;
        memcpy(&v1, p, 8); p += 8;
        memcpy(&v2, p, 8); p += 8;
        memcpy(&v3, p, 8); p += 8;
        memcpy(&v4, p, 8); p += 8;

        /* XOR data into state */
        h1 ^= v1; h2 ^= v2; h3 ^= v3; h4 ^= v4;

        /* S-box mixing (6× AND/OR/XOR = 6 ops, all 1-cycle) */
        sbox_64(&h1, &h2, &h3, &h4);

        /* GF multiply by PHI (replaces integer multiply) */
        h1 = gf_mul(h1, PHI);
        h2 = gf_mul(h2, PHI);
        h3 = gf_mul(h3, PHI);
        h4 = gf_mul(h4, PHI);

        /* Rotate for bit diffusion */
        h1 = rot(h1, 11);
        h2 = rot(h2, 19);
        h3 = rot(h3, 35);
        h4 = rot(h4, 47);
    }

    /* Tail handling */
    while (p + 8 <= e) {
        uint64_t v; memcpy(&v, p, 8); p += 8;
        h1 ^= v;
        sbox_64(&h1, &h2, &h3, &h4);
        h1 = gf_mul(h1, PHI);
        h1 = rot(h1, 11);
    }
    uint64_t t = 0;
    switch (e - p) {
        case 7: t ^= (uint64_t)p[6] << 48; case 6: t ^= (uint64_t)p[5] << 40;
        case 5: t ^= (uint64_t)p[4] << 32; case 4: t ^= (uint64_t)p[3] << 24;
        case 3: t ^= (uint64_t)p[2] << 16; case 2: t ^= (uint64_t)p[1] << 8;
        case 1: t ^= p[0]; h1 ^= t;
    }

    /* Final mixing */
    h1 ^= h2; h3 ^= h4; h1 ^= h3;
    sbox_64(&h1, &h2, &h3, &h4);
    h1 = gf_mul(h1, PHI);
    h1 ^= h1 >> 29; h1 = gf_mul(h1, PHI);
    h1 ^= h1 >> 31; h1 = gf_mul(h1, PHI);
    h1 ^= h1 >> 37; h1 = gf_mul(h1, PHI);
    h1 ^= h1 >> 41;

    return h1;
}

/* Reference v2 for comparison */
static inline uint64_t rotl(uint64_t x,int k){return(x<<k)|(x>>(64-k));}
#define M1 0x85EBCA77C2B2AE3DULL
#define M2 0xBF58476D1CE4E5B9ULL
#define M3 0x94D049BB133111EBULL
uint64_t v2(const void*b,size_t l,uint64_t s){const uint8_t*p=b,*e=p+l;uint64_t ba=s^(l*PHI),h1=ba*PHI,h2=ba*M1,h3=ba*M2,h4=ba*M3;while(p+32<=e){h1^=rotl(*(const uint64_t*)p,11);h1*=PHI;p+=8;h2^=rotl(*(const uint64_t*)p,19);h2*=PHI;p+=8;h3^=rotl(*(const uint64_t*)p,35);h3*=PHI;p+=8;h4^=rotl(*(const uint64_t*)p,47);h4*=PHI;p+=8;}while(p+8<=e){h1^=rotl(*(const uint64_t*)p,11);h1*=PHI;p+=8;}uint64_t t=0;switch(e-p){case 7:t^=(uint64_t)p[6]<<48;case 6:t^=(uint64_t)p[5]<<40;case 5:t^=(uint64_t)p[4]<<32;case 4:t^=(uint64_t)p[3]<<24;case 3:t^=(uint64_t)p[2]<<16;case 2:t^=(uint64_t)p[1]<<8;case 1:t^=p[0];h1^=rotl(t,11);h1*=PHI;}h1^=h2;h3^=h4;h1^=h3;h1^=h1>>29;h1*=M1;h1^=h1>>31;h1*=M2;h1^=h1>>37;h1*=PHI;h1^=h1>>41;return h1;}

static int pop(uint64_t x){x-=x>>1&0x5555555555555555ULL;x=(x&0x3333333333333333ULL)+(x>>2&0x3333333333333333ULL);x=(x+(x>>4))&0x0F0F0F0F0F0F0F0FULL;return(x*0x0101010101010101ULL)>>56;}

int main(void){
    printf("=== Saturnin-inspired vs v2 ===\n\n");
    uint8_t wb[1024];volatile uint64_t ws=0;for(int i=0;i<1000;i++){ws^=v2(wb,256,i);ws^=v_sat(wb,256,i);}

    printf("--- SPEED ---\n%8s","");size_t szs[]={64,256,1024,8192,65536};int ns=5;
    struct{const char*n;void*f;}hs[]={{"v2(mul)",v2},{"v_sat",v_sat}};int nh=2;
    for(int si=0;si<ns;si++)printf(" %8zu",szs[si]);printf("\n");
    for(int hi=0;hi<nh;hi++){printf("%-8s",hs[hi].n);typedef uint64_t(*hf)(const void*,size_t,uint64_t);hf fn=(hf)hs[hi].f;
        for(int si=0;si<ns;si++){size_t sz=szs[si];uint8_t*b=malloc(sz);for(size_t i=0;i<sz;i++)b[i]=i*0x9D+0x37;
            int it=sz<1024?50000000/sz:500000;if(it<10)it=10;volatile uint64_t s=0;clock_t st=clock();
            for(int i=0;i<it;i++)s^=fn(b,sz,i);clock_t en=clock();
            printf(" %8.1f",sz*(double)it/((double)(en-st)/CLOCKS_PER_SEC)/1e9);free(b);}printf(" GB/s\n");}

    printf("\n--- AVALANCHE ---\n");
    for(int hi=0;hi<nh;hi++){typedef uint64_t(*hf)(const void*,size_t,uint64_t);hf fn=(hf)hs[hi].f;
        uint8_t buf[256];for(int i=0;i<256;i++)buf[i]=i*0x9D+0x37;uint64_t base=fn(buf,256,0);double lo=100;
        for(int by=0;by<32;by++)for(int bi=0;bi<8;bi++){buf[by]^=(1u<<bi);uint64_t hv=fn(buf,256,0);buf[by]^=(1u<<bi);
            double p=pop(base^hv)/64.0*100;if(p<lo)lo=p;}
        printf("%-8s worst=%.1f%%\n",hs[hi].n,lo);}

    printf("\n--- CHI2 ---\n");
    for(int hi=0;hi<nh;hi++){typedef uint64_t(*hf)(const void*,size_t,uint64_t);hf fn=(hf)hs[hi].f;
        int bins[256]={0};for(int i=0;i<256000;i++)bins[fn(&i,4,i)&255]++;
        double ex=1000.0,chi2=0;for(int b=0;b<256;b++){double d=bins[b]-ex;chi2+=d*d/ex;}
        printf("%-8s chi2=%.1f\n",hs[hi].n,chi2);}
    return 0;
}
