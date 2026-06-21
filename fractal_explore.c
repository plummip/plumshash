/*
 * fractal_exploration.c — Fractal-inspired hash mixing techniques
 * ================================================================
 *
 * Techniques explored:
 *   1. Julia-set iteration: z = z² + c (quadratic chaotic map)
 *   2. Cantor diagonal pass: interleaved byte-level mixing
 *   3. Scale-recursive finalizer: main loop pattern reapplied to state
 *   4. Self-similar multi-resolution: same mixing at 64b/32b/16b/8b
 *
 * Compares against VortexHash baseline.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#include <math.h>

static inline uint64_t rot(uint64_t x,int k){return(x<<k)|(x>>(64-k));}
static inline int pop(uint64_t x){x-=x>>1&0x5555555555555555ULL;x=(x&0x3333333333333333ULL)+(x>>2&0x3333333333333333ULL);x=(x+(x>>4))&0x0F0F0F0F0F0F0F0FULL;return(x*0x0101010101010101ULL)>>56;}

#define PHI 0x9E3779B97F4A7C15ULL
#define M1  0x85EBCA77C2B2AE3DULL
#define M2  0xBF58476D1CE4E5B9ULL
#define M3  0x94D049BB133111EBULL

/* ─── BASELINE: VortexHash ─── */
uint64_t vortex(const void*b,size_t l,uint64_t s){
    const uint8_t*p=b,*e=p+l;uint64_t ba=s^(l*PHI),h1=ba*PHI,h2=ba*M1,h3=ba*M2,h4=ba*M3;
    while(p+32<=e){h1^=*(const uint64_t*)p;h1=rot(h1+h2,11);p+=8;h2^=*(const uint64_t*)p;h2=rot(h2+h3,17);p+=8;h3^=*(const uint64_t*)p;h3=rot(h3+h4,23);p+=8;h4^=*(const uint64_t*)p;h4=rot(h4+h1,57);p+=8;}
    while(p+8<=e){h1^=*(const uint64_t*)p;h1=rot(h1+h2,11);p+=8;}
    uint64_t t=0;switch(e-p){case 7:t^=(uint64_t)p[6]<<48;case 6:t^=(uint64_t)p[5]<<40;case 5:t^=(uint64_t)p[4]<<32;case 4:t^=(uint64_t)p[3]<<24;case 3:t^=(uint64_t)p[2]<<16;case 2:t^=(uint64_t)p[1]<<8;case 1:t^=p[0];h1^=t;h1=rot(h1+h2,11);}
    h1^=h2;h3^=h4;h1^=h3;h1^=h1>>29;h1*=M1;h1^=h1>>31;h1*=M2;h1^=h1>>37;h1*=PHI;h1^=h1>>41;return h1;
}

/* ─── JULIA: z = z² + c chaotic iteration ───
 * Uses 128-bit multiply (like wyhash) for the z² step,
 * then XORs a constant.  This is a discrete chaotic map. */
static inline uint64_t julia_mix(uint64_t z, uint64_t c) {
    __uint128_t sq = (__uint128_t)z * z;
    z = (uint64_t)sq ^ (uint64_t)(sq >> 64);  /* z = z² in GF sense */
    z ^= c;                                    /* z = z² ^ c */
    return rot(z, 17);                         /* rotate for diffusion */
}

uint64_t julia(const void*b,size_t l,uint64_t s){
    const uint8_t*p=b,*e=p+l;
    uint64_t h1=s^PHI,h2=s^M1,h3=s^M2,h4=s^(l*PHI);
    while(p+32<=e){
        h1=julia_mix(h1^*(const uint64_t*)p,PHI);p+=8;
        h2=julia_mix(h2^*(const uint64_t*)p,M1);p+=8;
        h3=julia_mix(h3^*(const uint64_t*)p,M2);p+=8;
        h4=julia_mix(h4^*(const uint64_t*)p,PHI);p+=8;
    }
    while(p+8<=e){h1=julia_mix(h1^*(const uint64_t*)p,PHI);p+=8;}
    uint64_t t=0;switch(e-p){case 7:t^=(uint64_t)p[6]<<48;case 6:t^=(uint64_t)p[5]<<40;case 5:t^=(uint64_t)p[4]<<32;case 4:t^=(uint64_t)p[3]<<24;case 3:t^=(uint64_t)p[2]<<16;case 2:t^=(uint64_t)p[1]<<8;case 1:t^=p[0];h1=julia_mix(h1^t,PHI);}
    h1^=h2;h3^=h4;h1^=h3;
    h1=julia_mix(h1,PHI);h1=julia_mix(h1,M1);h1=julia_mix(h1,M2);h1=julia_mix(h1,PHI);
    return h1;
}

/* ─── CANTOR: diagonal pass after main loop ───
 * After block processing, do a Cantor-set inspired pass:
 * mix bytes at positions 0,2,4,... then 1,3,5,... into the state.
 * This is O(n) extra but creates fractal self-similarity. */
uint64_t cantor(const void*b,size_t l,uint64_t s){
    const uint8_t*p=b,*e=p+l;uint64_t ba=s^(l*PHI),h1=ba*PHI,h2=ba*M1,h3=ba*M2,h4=ba*M3;
    while(p+32<=e){h1^=*(const uint64_t*)p;h1=rot(h1+h2,11);p+=8;h2^=*(const uint64_t*)p;h2=rot(h2+h3,17);p+=8;h3^=*(const uint64_t*)p;h3=rot(h3+h4,23);p+=8;h4^=*(const uint64_t*)p;h4=rot(h4+h1,57);p+=8;}
    /* Cantor diagonal: mix alternating bytes */
    const uint8_t *bp=b;
    for(size_t i=0;i<l;i+=2){h1^=(uint64_t)bp[i]<<(8*(i%8));h1=rot(h1+h2,13);}
    for(size_t i=1;i<l;i+=2){h2^=(uint64_t)bp[i]<<(8*(i%8));h2=rot(h2+h3,29);}
    while(p+8<=e){h1^=*(const uint64_t*)p;h1=rot(h1+h2,11);p+=8;}
    uint64_t t=0;switch(e-p){case 7:t^=(uint64_t)p[6]<<48;case 6:t^=(uint64_t)p[5]<<40;case 5:t^=(uint64_t)p[4]<<32;case 4:t^=(uint64_t)p[3]<<24;case 3:t^=(uint64_t)p[2]<<16;case 2:t^=(uint64_t)p[1]<<8;case 1:t^=p[0];h1^=t;h1=rot(h1+h2,11);}
    h1^=h2;h3^=h4;h1^=h3;h1^=h1>>29;h1*=M1;h1^=h1>>31;h1*=M2;h1^=h1>>37;h1*=PHI;h1^=h1>>41;return h1;
}

/* ─── FRACTAL FINALIZER: ARX finalizer mirrors main loop structure ───
 * Self-similarity: the finalizer uses the same ARX pattern as the
 * main loop, creating a fractal where the same transform applies
 * at both the data-processing and state-finalization levels. */
uint64_t fractal_fin(const void*b,size_t l,uint64_t s){
    const uint8_t*p=b,*e=p+l;uint64_t ba=s^(l*PHI),h1=ba*PHI,h2=ba*M1,h3=ba*M2,h4=ba*M3;
    while(p+32<=e){h1^=*(const uint64_t*)p;h1=rot(h1+h2,11);p+=8;h2^=*(const uint64_t*)p;h2=rot(h2+h3,17);p+=8;h3^=*(const uint64_t*)p;h3=rot(h3+h4,23);p+=8;h4^=*(const uint64_t*)p;h4=rot(h4+h1,57);p+=8;}
    while(p+8<=e){h1^=*(const uint64_t*)p;h1=rot(h1+h2,11);p+=8;}
    uint64_t t=0;switch(e-p){case 7:t^=(uint64_t)p[6]<<48;case 6:t^=(uint64_t)p[5]<<40;case 5:t^=(uint64_t)p[4]<<32;case 4:t^=(uint64_t)p[3]<<24;case 3:t^=(uint64_t)p[2]<<16;case 2:t^=(uint64_t)p[1]<<8;case 1:t^=p[0];h1^=t;h1=rot(h1+h2,11);}
    /* Fractal finalizer: same ARX pattern as main loop, applied to 4 lanes */
    h1=rot(h1+h2,11);h2=rot(h2+h3,17);h3=rot(h3+h4,23);h4=rot(h4+h1,57);
    h1=rot(h1+h2,13);h2=rot(h2+h3,19);h3=rot(h3+h4,31);h4=rot(h4+h1,43);
    h1=rot(h1+h2,17);h2=rot(h2+h3,23);h3=rot(h3+h4,37);h4=rot(h4+h1,53);
    h1^=h2;h3^=h4;h1^=h3;
    h1^=h1>>29;h1*=M1;h1^=h1>>31;h1*=M2;h1^=h1>>37;h1*=PHI;h1^=h1>>41;return h1;
}

/* ─── MULTI-RESOLUTION: self-similar mixing at 64b → 32b → 16b → 8b ───
 * The same ARX pattern applied at decreasing word sizes.
 * This is fractal: the same structure at different scales. */
uint64_t multires(const void*b,size_t l,uint64_t s){
    const uint8_t*p=b,*e=p+l;uint64_t ba=s^(l*PHI),h1=ba*PHI,h2=ba*M1,h3=ba*M2,h4=ba*M3;
    while(p+32<=e){h1^=*(const uint64_t*)p;h1=rot(h1+h2,11);p+=8;h2^=*(const uint64_t*)p;h2=rot(h2+h3,17);p+=8;h3^=*(const uint64_t*)p;h3=rot(h3+h4,23);p+=8;h4^=*(const uint64_t*)p;h4=rot(h4+h1,57);p+=8;}
    /* Multi-resolution: re-process state as 4×32-bit pairs */
    uint32_t *s32=(uint32_t*)&h1;
    h1=rot(h1+s32[0],11);h2=rot(h2+s32[1],17);h3=rot(h3+s32[2],23);h4=rot(h4+s32[3],57);
    /* Multi-resolution: re-process as 8×16-bit */
    uint16_t *s16=(uint16_t*)&h1;
    h1=rot(h1+s16[0],13);h2=rot(h2+s16[1],19);h3=rot(h3+s16[2],31);h4=rot(h4+s16[3],43);
    /* Multi-resolution: re-process as 16×8-bit */
    uint8_t *s8=(uint8_t*)&h1;
    for(int i=0;i<8;i++){h1^=s8[i];h1=rot(h1+h2,17);}
    while(p+8<=e){h1^=*(const uint64_t*)p;h1=rot(h1+h2,11);p+=8;}
    uint64_t t=0;switch(e-p){case 7:t^=(uint64_t)p[6]<<48;case 6:t^=(uint64_t)p[5]<<40;case 5:t^=(uint64_t)p[4]<<32;case 4:t^=(uint64_t)p[3]<<24;case 3:t^=(uint64_t)p[2]<<16;case 2:t^=(uint64_t)p[1]<<8;case 1:t^=p[0];h1^=t;h1=rot(h1+h2,11);}
    h1^=h2;h3^=h4;h1^=h3;h1^=h1>>29;h1*=M1;h1^=h1>>31;h1*=M2;h1^=h1>>37;h1*=PHI;h1^=h1>>41;return h1;
}

int main(void){
    printf("=== Fractal-Inspired Hash Exploration ===\n\n");
    struct{const char*n;void*f;}hs[]={
        {"Vortex",vortex},{"Julia",julia},{"Cantor",cantor},
        {"FractalFin",fractal_fin},{"MultiRes",multires}};
    int nh=5;

    /* warmup */
    uint8_t wb[256];volatile uint64_t ws=0;
    for(int i=0;i<1000;i++)for(int j=0;j<nh;j++)
        {typedef uint64_t(*hf)(const void*,size_t,uint64_t);hf fn=(hf)hs[j].f;ws^=fn(wb,256,i);}

    /* SPEED */
    printf("--- SPEED (GB/s) ---\n%10s","");
    size_t szs[]={64,256,1024,8192,65536};int ns=5;
    for(int si=0;si<ns;si++)printf(" %8zu",szs[si]);printf("\n");
    for(int hi=0;hi<nh;hi++){
        printf("%-10s",hs[hi].n);
        typedef uint64_t(*hf)(const void*,size_t,uint64_t);hf fn=(hf)hs[hi].f;
        for(int si=0;si<ns;si++){size_t sz=szs[si];uint8_t*b=malloc(sz);
            for(size_t i=0;i<sz;i++)b[i]=i*0x9D+0x37;
            int it=sz<1024?50000000/sz:500000;if(it<10)it=10;
            volatile uint64_t s=0;clock_t st=clock();
            for(int i=0;i<it;i++)s^=fn(b,sz,i);clock_t en=clock();
            printf(" %8.1f",sz*(double)it/((double)(en-st)/CLOCKS_PER_SEC)/1e9);free(b);
        }printf(" GB/s\n");
    }

    /* QUALITY */
    printf("\n--- QUALITY ---\n%10s %6s %8s\n","","ava%","chi2");
    for(int hi=0;hi<nh;hi++){
        typedef uint64_t(*hf)(const void*,size_t,uint64_t);hf fn=(hf)hs[hi].f;
        uint8_t buf[256];for(int i=0;i<256;i++)buf[i]=i*0x9D+0x37;
        uint64_t base=fn(buf,256,0);double lo=100;
        for(int by=0;by<32;by++)for(int bi=0;bi<8;bi++){buf[by]^=(1u<<bi);uint64_t hv=fn(buf,256,0);buf[by]^=(1u<<bi);
            double p=pop(base^hv)/64.0*100;if(p<lo)lo=p;}
        int bins[256]={0};for(int i=0;i<256000;i++)bins[fn(&i,4,i)&255]++;
        double ex=1000.0,chi2=0;for(int b=0;b<256;b++){double d=bins[b]-ex;chi2+=d*d/ex;}
        printf("%-10s %5.1f%% %7.1f\n",hs[hi].n,lo,chi2);
    }
    return 0;
}
