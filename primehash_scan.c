/*
 * primehash_scan.c — Scan rotation sets for best avalanche worst-case
 * ===================================================================
 * Tests candidate rotation sets for PrimeHash v1 body.
 * Keeps multipliers and finalizer unchanged.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>

static inline uint64_t read64(const void *p){uint64_t v;memcpy(&v,p,8);return v;}
static inline uint64_t rotl64(uint64_t x,int k){return(x<<k)|(x>>(64-k));}
static inline int popcount64(uint64_t x){x-=x>>1&0x5555555555555555ULL;x=(x&0x3333333333333333ULL)+(x>>2&0x3333333333333333ULL);x=(x+(x>>4))&0x0F0F0F0F0F0F0F0FULL;return(x*0x0101010101010101ULL)>>56;}

#define PHI 0x9E3779B97F4A7C15ULL
#define M1  0x85EBCA77C2B2AE3DULL
#define M2  0xBF58476D1CE4E5B9ULL
#define M3  0x94D049BB133111EBULL

static uint64_t hash_with_rots(const void *buf, size_t len, uint64_t seed,
                                int r1, int r2, int r3, int r4){
    const uint8_t *p=(const uint8_t*)buf,*end=p+len;
    uint64_t base=seed^(len*PHI);
    uint64_t h1=base*PHI,h2=base*M1,h3=base*M2,h4=base*M3;
    while(p+32<=end){
        h1^=rotl64(read64(p),r1);h1*=PHI;p+=8;
        h2^=rotl64(read64(p),r2);h2*=PHI;p+=8;
        h3^=rotl64(read64(p),r3);h3*=PHI;p+=8;
        h4^=rotl64(read64(p),r4);h4*=PHI;p+=8;
    }
    while(p+8<=end){h1^=rotl64(read64(p),r1);h1*=PHI;p+=8;}
    uint64_t tail=0;
    switch(end-p){
        case 7:tail^=(uint64_t)p[6]<<48;case 6:tail^=(uint64_t)p[5]<<40;
        case 5:tail^=(uint64_t)p[4]<<32;case 4:tail^=(uint64_t)p[3]<<24;
        case 3:tail^=(uint64_t)p[2]<<16;case 2:tail^=(uint64_t)p[1]<<8;
        case 1:tail^=(uint64_t)p[0];h1^=rotl64(tail,r1);h1*=PHI;
    }
    h1^=h2;h3^=h4;h1^=h3;
    h1^=h1>>29;h1*=M1;h1^=h1>>31;h1*=M2;h1^=h1>>37;h1*=PHI;h1^=h1>>41;
    return h1;
}

typedef struct {int r1,r2,r3,r4; double avg,worst,best; int fail;} Result;

int main(void){
    /* Candidate rotation sets: odd numbers 11-61, pick 4 */
    int candidates[] = {11,13,15,17,19,21,23,25,27,29,31,33,35,37,39,41,43,45,47,49,51,53,55,57,59,61};
    int nc = sizeof(candidates)/sizeof(candidates[0]);
    Result best = {0}; best.worst = 0;

    printf("Scanning rotation sets for best avalanche worst-case...\n");
    printf("(format: r1,r2,r3,r4 | avg worst best | status)\n\n");

    int tested = 0, good = 0;
    uint8_t buf[256];
    for(int i=0;i<256;i++) buf[i]=(uint8_t)(i*0x9D+0x37);

    /* Test each combination (4 nested loops — ~150K combinations) */
    for(int a=0;a<nc;a++){
    for(int b=a+1;b<nc;b++){
    for(int c=b+1;c<nc;c++){
    for(int d=c+1;d<nc;d++){
        int r1=candidates[a],r2=candidates[b],r3=candidates[c],r4=candidates[d];

        /* Skip sets where any pair sums to 64 (inverse rotations) */
        if(r1+r2==64||r1+r3==64||r1+r4==64||r2+r3==64||r2+r4==64||r3+r4==64) continue;
        /* Skip sets where any pair sums to 60 (current issue) */
        /* Actually let's not skip — let's see if 60-sum is bad */

        uint64_t base = hash_with_rots(buf,256,0,r1,r2,r3,r4);
        double sum=0, lo=100, hi=0;
        int fail=0;

        for(int by=0;by<32&&!fail;by++){
        for(int bi=0;bi<8&&!fail;bi++){
            buf[by]^=(1u<<bi);
            uint64_t hv=hash_with_rots(buf,256,0,r1,r2,r3,r4);
            buf[by]^=(1u<<bi);
            double pct = popcount64(base^hv)/64.0*100.0;
            sum+=pct;
            if(pct<lo)lo=pct;
            if(pct>hi)hi=pct;
            if(pct<25.0) fail=1;  /* reject really bad ones early */
        }}

        if(!fail){
            double avg=sum/(32*8);
            if(lo > best.worst){
                best.r1=r1; best.r2=r2; best.r3=r3; best.r4=r4;
                best.avg=avg; best.worst=lo; best.best=hi;
            }
            if(lo >= 35.0){  /* better than current 35.9% */
                printf("  {%2d,%2d,%2d,%2d} | avg=%.1f%% worst=%.1f%% best=%.1f%% | GOOD\n",
                       r1,r2,r3,r4,avg,lo,hi);
                good++;
            }
            /* Print top 20 even if not > 35% */
            static int printed=0;
            if(printed<20 && lo>30){
                printf("  {%2d,%2d,%2d,%2d} | avg=%.1f%% worst=%.1f%% best=%.1f%%\n",
                       r1,r2,r3,r4,avg,lo,hi);
                printed++;
            }
        }
        tested++;
    }}}}

    printf("\n=== SCAN COMPLETE ===\n");
    printf("Tested: %d combinations\n", tested);
    printf("Best: {%d,%d,%d,%d} avg=%.1f%% worst=%.1f%% best=%.1f%%\n",
           best.r1,best.r2,best.r3,best.r4,best.avg,best.worst,best.best);
    printf("Current v1: {23,47,13,37} — let me check this exact set...\n");

    /* Test current v1 rotations */
    uint64_t base = hash_with_rots(buf,256,0,23,47,13,37);
    double sum=0, lo=100, hi=0;
    for(int by=0;by<32;by++){
    for(int bi=0;bi<8;bi++){
        buf[by]^=(1u<<bi);
        uint64_t hv=hash_with_rots(buf,256,0,23,47,13,37);
        buf[by]^=(1u<<bi);
        double pct = popcount64(base^hv)/64.0*100.0;
        sum+=pct; if(pct<lo)lo=pct; if(pct>hi)hi=pct;
    }}
    printf("v1 {23,47,13,37}: avg=%.1f%% worst=%.1f%% best=%.1f%%\n", sum/(32*8), lo, hi);

    return 0;
}
