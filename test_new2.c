#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#include <math.h>

/* ── NEW version inline ── */
static inline uint64_t nr_rot(uint64_t x, int k) { return (x << k) | (x >> (64 - k)); }
static inline uint64_t nr_read64(const uint8_t *p) { uint64_t v; memcpy(&v, p, sizeof(v)); return v; }
#define NR_PHI 0x9E3779B97F4A7C15ULL
#define NR_M1  0x85EBCA77C2B2AE3DULL
#define NR_M2  0xBF58476D1CE4E5B9ULL
#define NR_M3  0x94D049BB133111EBULL

static uint64_t plum_fast_new(const uint8_t *p, size_t len, uint64_t seed) {
    const uint8_t *e = p + len;
    uint64_t ba = seed ^ (len * NR_PHI);
    uint64_t h1 = ba * NR_PHI, h2 = ba * NR_M1, h3 = ba * NR_M2, h4 = ba * NR_M3;
    while (p + 32 <= e) {
        uint64_t v1 = nr_read64(p); p += 8; uint64_t v2 = nr_read64(p); p += 8;
        uint64_t v3 = nr_read64(p); p += 8; uint64_t v4 = nr_read64(p); p += 8;
        uint64_t t1 = nr_rot((h1 ^ v1) + h2, 11); uint64_t t2 = nr_rot((h2 ^ v2) + h3, 17);
        uint64_t t3 = nr_rot((h3 ^ v3) + h4, 23); uint64_t t4 = nr_rot((h4 ^ v4) + h1, 57);
        h1 = t1; h2 = t2; h3 = t3; h4 = t4;
    }
    while (p + 8 <= e) { uint64_t v = nr_read64(p); p += 8; h1 ^= v; h1 = nr_rot(h1 + h2, 11); }
    h2 ^= h1; h2 = nr_rot(h2, 33); h1 ^= h2;
    uint64_t t = 0;
    switch (e - p) { case 7: t ^= (uint64_t)p[6] << 48; case 6: t ^= (uint64_t)p[5] << 40; case 5: t ^= (uint64_t)p[4] << 32; case 4: t ^= (uint64_t)p[3] << 24; case 3: t ^= (uint64_t)p[2] << 16; case 2: t ^= (uint64_t)p[1] << 8; case 1: t ^= p[0]; t *= NR_PHI; h1 ^= t; h1 = nr_rot(h1 + h2, 11); }
    h1 ^= h2; h3 ^= h4; h1 ^= h3;
    h1 ^= h1 >> 29; h1 *= NR_M1; h1 ^= h1 >> 31; h1 *= NR_M2; h1 ^= h1 >> 37; h1 *= NR_PHI; h1 ^= h1 >> 41;
    h1 ^= h1 >> 43; h1 *= NR_M3; h1 ^= h1 >> 47;
    return h1;
}
static uint64_t plum_safe_new(const uint8_t *p, size_t len, uint64_t seed) {
    const uint8_t *e = p + len; uint64_t ba = seed ^ (len * NR_PHI);
    uint64_t h1 = ba * NR_PHI, h2 = ba * NR_M1, h3 = ba * NR_M2, h4 = ba * NR_M3, acc = ba ^ NR_M2; int has_blocks = 0;
    while (p + 32 <= e) {
        uint64_t v1 = nr_read64(p); p += 8; uint64_t v2 = nr_read64(p); p += 8;
        uint64_t v3 = nr_read64(p); p += 8; uint64_t v4 = nr_read64(p); p += 8;
        uint64_t t1 = nr_rot((h1 ^ v1) + h2, 11); uint64_t t2 = nr_rot((h2 ^ v2) + h3, 17);
        uint64_t t3 = nr_rot((h3 ^ v3) + h4, 23); uint64_t t4 = nr_rot((h4 ^ v4) + h1, 57);
        h1 = t1; h2 = t2; h3 = t3; h4 = t4;
        acc ^= v1 ^ v2 ^ v3 ^ v4; acc = nr_rot(acc, 31); acc *= NR_PHI; has_blocks = 1;
    }
    while (p + 8 <= e) { uint64_t v = nr_read64(p); p += 8; h1 ^= v; h1 = nr_rot(h1 + h2, 11); acc ^= v; acc = nr_rot(acc, 31); acc *= NR_PHI; has_blocks = 1; }
    h2 ^= h1; h2 = nr_rot(h2, 33); h1 ^= h2;
    uint64_t t = 0;
    switch (e - p) { case 7: t ^= (uint64_t)p[6] << 48; case 6: t ^= (uint64_t)p[5] << 40; case 5: t ^= (uint64_t)p[4] << 32; case 4: t ^= (uint64_t)p[3] << 24; case 3: t ^= (uint64_t)p[2] << 16; case 2: t ^= (uint64_t)p[1] << 8; case 1: t ^= p[0]; t *= NR_PHI; h1 ^= t; h1 = nr_rot(h1 + h2, 11); if (has_blocks) { acc ^= t; acc = nr_rot(acc, 31); acc *= NR_PHI; } }
    h1 ^= h2; h3 ^= h4; h1 ^= h3; if (has_blocks) h1 ^= acc;
    h1 ^= h1 >> 29; h1 *= NR_M1; h1 ^= h1 >> 31; h1 *= NR_M2; h1 ^= h1 >> 37; h1 *= NR_PHI; h1 ^= h1 >> 41;
    h1 ^= h1 >> 43; h1 *= NR_M3; h1 ^= h1 >> 47;
    return h1;
}
uint64_t new_hash(const void *b, size_t l, uint64_t s) { return l>=256 ? plum_fast_new((const uint8_t*)b,l,s) : plum_safe_new((const uint8_t*)b,l,s); }

/* ── ORIGINAL ── */
#define PLUMHASH_IMPLEMENTATION
#include "plumhash.h"

static inline int pop(uint64_t x){x-=x>>1&0x5555555555555555ULL;x=(x&0x3333333333333333ULL)+(x>>2&0x3333333333333333ULL);x=(x+(x>>4))&0x0F0F0F0F0F0F0F0FULL;return(x*0x0101010101010101ULL)>>56;}
typedef uint64_t(*hf)(const void*,size_t,uint64_t);

int main(void) {
    printf("=== NEW vs ORIGINAL ===\n\n");
    struct{const char*n;hf f;} hs[]={{"ORIGINAL",plumhash},{"NEW",new_hash}}; int nh=2;
    uint8_t wb[256]; volatile uint64_t ws=0;
    for(int i=0;i<500;i++) for(int j=0;j<nh;j++) ws^=hs[j].f(wb,256,i);
    printf("%-10s %6s %7s %6s %8s %8s\n","","ava%","chi2","sparse","64B","1KB");
    for(int hi=0;hi<nh;hi++) { hf fn = hs[hi].f;
        uint8_t buf[256]; for(int i=0;i<256;i++) buf[i]=i*0x9D+0x37; uint64_t base=fn(buf,256,0); double lo=100;
        for(int by=0;by<32;by++) for(int bi=0;bi<8;bi++) { buf[by]^=(1u<<bi); uint64_t hv=fn(buf,256,0); buf[by]^=(1u<<bi); double p=pop(base^hv)/64.0*100; if(p<lo) lo=p; }
        int bins[256]={0}; for(int i=0;i<256000;i++) bins[fn(&i,4,i)&255]++; double ex=1000.0,chi2=0; for(int b=0;b<256;b++){double d=bins[b]-ex;chi2+=d*d/ex;}
        int cols=0; uint64_t seen[20000]={0}; int ngen=0; uint8_t key[256]; for(int pos=0;pos<128&&ngen<20000;pos++) for(int val=1;val<256&&ngen<20000;val++) { int kl=8+(pos%57); memset(key,0,kl); key[kl-1]=val; if(pos&1) key[0]=pos^val; seen[ngen++]=fn(key,kl,0); } for(int i=0;i<ngen;i++) for(int j=i+1;j<ngen;j++) if(seen[i]==seen[j]) cols++;
        double spd64=0,spd1k=0;
        { size_t sz=64; uint8_t*b=malloc(sz); for(size_t i=0;i<sz;i++) b[i]=i*0x9D+0x37; volatile uint64_t s=0; clock_t st=clock(); for(int i=0;i<20000000/sz;i++) s^=fn(b,sz,i); clock_t en=clock(); spd64=sz*(20000000/sz)/((double)(en-st)/CLOCKS_PER_SEC)/1e9; free(b); }
        { size_t sz=1024; uint8_t*b=malloc(sz); for(size_t i=0;i<sz;i++) b[i]=i*0x9D+0x37; volatile uint64_t s=0; clock_t st=clock(); for(int i=0;i<20000000/sz;i++) s^=fn(b,sz,i); clock_t en=clock(); spd1k=sz*(20000000/sz)/((double)(en-st)/CLOCKS_PER_SEC)/1e9; free(b); }
        printf("%-10s %5.1f%% %7.1f %5d %7.1f %7.1f\n",hs[hi].n,lo,chi2,cols,spd64,spd1k);
    }
    return 0;
}
