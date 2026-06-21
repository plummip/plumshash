/*
 * scan_mulmix.c — Scan rotations for mulmix-based PrimeHash
 * ==========================================================
 * Uses 128-bit multiply (lo^hi) in the body loop.
 * Finds best rotation set for this mixing primitive.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

static inline uint64_t rotl64(uint64_t x,int k){return(x<<k)|(x>>(64-k));}
#define PHI 0x9E3779B97F4A7C15ULL
#define M1 0x85EBCA77C2B2AE3DULL
#define M2 0xBF58476D1CE4E5B9ULL
#define M3 0x94D049BB133111EBULL

static inline uint64_t mulmix(uint64_t a,uint64_t b){
    __uint128_t p=(__uint128_t)a*b;
    return (uint64_t)p^(uint64_t)(p>>64);
}

static uint64_t hash_mm(const void *b,size_t l,uint64_t s,int r1,int r2,int r3,int r4){
    const uint8_t*p=(const uint8_t*)b,*e=p+l;
    uint64_t ba=s^(l*PHI),h1=ba*PHI,h2=ba*M1,h3=ba*M2,h4=ba*M3;
    while(p+32<=e){
        h1^=rotl64(*(const uint64_t*)p,r1);h1=mulmix(h1,PHI);p+=8;
        h2^=rotl64(*(const uint64_t*)p,r2);h2=mulmix(h2,PHI);p+=8;
        h3^=rotl64(*(const uint64_t*)p,r3);h3=mulmix(h3,PHI);p+=8;
        h4^=rotl64(*(const uint64_t*)p,r4);h4=mulmix(h4,PHI);p+=8;
    }
    while(p+8<=e){h1^=rotl64(*(const uint64_t*)p,r1);h1=mulmix(h1,PHI);p+=8;}
    uint64_t t=0;switch(e-p){
        case 7:t^=(uint64_t)p[6]<<48;case 6:t^=(uint64_t)p[5]<<40;case 5:t^=(uint64_t)p[4]<<32;
        case 4:t^=(uint64_t)p[3]<<24;case 3:t^=(uint64_t)p[2]<<16;case 2:t^=(uint64_t)p[1]<<8;
        case 1:t^=p[0];h1^=rotl64(t,r1);h1=mulmix(h1,PHI);
    }
    h1^=h2;h3^=h4;h1^=h3;h1=mulmix(h1,PHI);
    h1^=h1>>17;h1=mulmix(h1,M1);h1^=h1>>23;h1=mulmix(h1,M2);
    h1^=h1>>29;h1=mulmix(h1,M3);h1^=h1>>37;h1=mulmix(h1,PHI);h1^=h1>>43;
    return h1;
}

static int pop(uint64_t x){x-=x>>1&0x5555555555555555ULL;x=(x&0x3333333333333333ULL)+(x>>2&0x3333333333333333ULL);x=(x+(x>>4))&0x0F0F0F0F0F0F0F0FULL;return(x*0x0101010101010101ULL)>>56;}

int main(void){
    int cand[]={11,13,15,17,19,21,23,25,27,29,31,33,35,37,39,41,43,45,47,49,51,53,55,57,59,61};
    int nc=sizeof(cand)/sizeof(cand[0]);
    uint8_t buf[256];for(int i=0;i<256;i++)buf[i]=i*0x9D+0x37;

    printf("Scanning rotation sets for mulmix (128-bit multiply)...\n");
    printf("%% = avalanche worst case (target: >35%%)\n\n");

    int best_worst=0,tested=0;
    int br1=0,br2=0,br3=0,br4=0;
    double bavg=0,bbest=0;

    for(int a=0;a<nc;a++){int r1=cand[a];
    for(int b=a+1;b<nc;b++){int r2=cand[b];
    for(int c=b+1;c<nc;c++){int r3=cand[c];
    for(int d=c+1;d<nc;d++){int r4=cand[d];
        if(r1+r2==64||r1+r3==64||r1+r4==64||r2+r3==64||r2+r4==64||r3+r4==64)continue;
        uint64_t base=hash_mm(buf,256,0,r1,r2,r3,r4);
        double s=0,lo=100,hi=0;int fail=0;
        for(int by=0;by<32&&!fail;by++)for(int bi=0;bi<8&&!fail;bi++){
            buf[by]^=(1u<<bi);uint64_t hv=hash_mm(buf,256,0,r1,r2,r3,r4);buf[by]^=(1u<<bi);
            double p=pop(base^hv)/64.0*100;s+=p;if(p<lo)lo=p;if(p>hi)hi=p;if(p<20) fail=1;
        }
        if(!fail){
            tested++;
            if((int)lo>best_worst){best_worst=(int)lo;br1=r1;br2=r2;br3=r3;br4=r4;bavg=s/(32*8);bbest=hi;}
            if(lo>=30.0)printf("{%2d,%2d,%2d,%2d} worst=%.1f%% avg=%.1f\n",r1,r2,r3,r4,lo,s/(32*8));
        }
    }}}}
    printf("\n=== BEST ===\n");
    printf("{%d,%d,%d,%d} worst=%.1f%% avg=%.1f best=%.1f\n",br1,br2,br3,br4,(double)best_worst,bavg,bbest);
    printf("Tested %d candidates\n",tested);
    return 0;
}
