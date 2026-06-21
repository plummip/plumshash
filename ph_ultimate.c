#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#include <math.h>

static inline uint64_t rot(uint64_t x,int k){return(x<<k)|(x>>(64-k));}
static inline int pop(uint64_t x){x-=x>>1&0x5555555555555555ULL;x=(x&0x3333333333333333ULL)+(x>>2&0x3333333333333333ULL);x=(x+(x>>4))&0x0F0F0F0F0F0F0F0FULL;return(x*0x0101010101010101ULL)>>56;}

/* Saturnin S-box on 4×64-bit lanes: nonlinear mixing using only AND/OR/XOR */
static inline void sbox(uint64_t *a,uint64_t *b,uint64_t *c,uint64_t *d){
    uint64_t ta=*a,tb=*b,tc=*c,td=*d;
    ta^=tb&tc; tb^=ta|td; td^=tb|tc; tc^=tb&td; tb^=ta|tc; ta^=tb|td;
    *a=tb;*b=tc;*c=td;*d=ta;
}

/* ─── ULTIMATE: 6 lanes (PRIEMFORMULE), ARX + S-box mixing ─── */
/* 6 safe residues mod 9 → 6 rotation constants */
#define U_R1 11
#define U_R2 17
#define U_R3 23
#define U_R4 29
#define U_R5 35
#define U_R6 43

uint64_t ultimate(const void*b,size_t l,uint64_t s){
    const uint8_t*p=(const uint8_t*)b,*e=p+l;
    /* 6 lanes initialized from seed */
    uint64_t z=s^(l*0x9E3779B97F4A7C15ULL);
    uint64_t h1=z*0x9E3779B97F4A7C15ULL;
    uint64_t h2=z*0x85EBCA77C2B2AE3DULL;
    uint64_t h3=z*0xBF58476D1CE4E5B9ULL;
    uint64_t h4=z*0x94D049BB133111EBULL;
    uint64_t h5=z*0xA3C8B9D5E1F2A7B4ULL;
    uint64_t h6=z*0xC6A4A7935BD1E995ULL;
    int ev=0; /* even/odd alternation (PRIEMFORMULE checkerboard) */

    /* 48 bytes per iteration: 6 lanes × 8 bytes */
    while(p+48<=e){
        /* Load 6 words, XOR into state */
        h1^=*(const uint64_t*)p;p+=8; h2^=*(const uint64_t*)p;p+=8;
        h3^=*(const uint64_t*)p;p+=8; h4^=*(const uint64_t*)p;p+=8;
        h5^=*(const uint64_t*)p;p+=8; h6^=*(const uint64_t*)p;p+=8;

        /* ARX mixing: add+rotate, all 1-cycle ops */
        h1=rot(h1+h4,U_R1);  /* cross-lane dependency chain */
        h2=rot(h2+h5,U_R2);
        h3=rot(h3+h6,U_R3);
        h4=rot(h4+h1,U_R4);
        h5=rot(h5+h2,U_R5);
        h6=rot(h6+h3,U_R6);

        /* S-box nonlinear mix: group lanes (1,2,3,4) then (3,4,5,6) */
        sbox(&h1,&h2,&h3,&h4);
        sbox(&h3,&h4,&h5,&h6);

        /* Alternating extra mix (PRIEMFORMULE checkerboard) */
        if(ev){
            h1=rot(h1+h3,13); h3=rot(h3+h1,31); /* even rows: pair (1,3) */
            h2=rot(h2+h4,19); h4=rot(h4+h2,37); /*           pair (2,4) */
            h5=rot(h5+h6,23); h6=rot(h6+h5,41); /*           pair (5,6) */
        }else{
            h1=rot(h1+h2,17); h2=rot(h2+h1,29); /* odd rows:  pair (1,2) */
            h3=rot(h3+h4,23); h4=rot(h4+h3,37); /*           pair (3,4) */
            h5=rot(h5+h6,31); h6=rot(h6+h5,43); /*           pair (5,6) */
        }
        ev^=1;
    }

    /* Tail: feed remaining 8-byte blocks to spread across lanes */
    int lc=0;
    while(p+8<=e){
        uint64_t v=*(const uint64_t*)p;p+=8;
        switch(lc%6){
            case 0:h1^=v;h1=rot(h1+h4,U_R1);break;
            case 1:h2^=v;h2=rot(h2+h5,U_R2);break;
            case 2:h3^=v;h3=rot(h3+h6,U_R3);break;
            case 3:h4^=v;h4=rot(h4+h1,U_R4);break;
            case 4:h5^=v;h5=rot(h5+h2,U_R5);break;
            case 5:h6^=v;h6=rot(h6+h3,U_R6);break;
        }
        lc++;
    }

    /* Tail bytes */
    uint64_t t=0;int tl=e-p;
    if(tl){switch(tl){case 7:t^=(uint64_t)p[6]<<48;case 6:t^=(uint64_t)p[5]<<40;case 5:t^=(uint64_t)p[4]<<32;case 4:t^=(uint64_t)p[3]<<24;case 3:t^=(uint64_t)p[2]<<16;case 2:t^=(uint64_t)p[1]<<8;case 1:t^=p[0];}
        switch(lc%6){
            case 0:h1^=t;h1=rot(h1+h4,11);break;case 1:h2^=t;h2=rot(h2+h5,17);break;
            case 2:h3^=t;h3=rot(h3+h6,23);break;case 3:h4^=t;h4=rot(h4+h1,29);break;
            case 4:h5^=t;h5=rot(h5+h2,35);break;case 5:h6^=t;h6=rot(h6+h3,43);break;
        }
    }

    /* Tree reduction 6→3→2→1 */
    h1^=h2;h3^=h4;h5^=h6;
    sbox(&h1,&h3,&h5,&h1); /* nonlinear reduction step 1 */
    h1^=h3;h1^=h5;
    sbox(&h1,&h1,&h1,&h1); /* nonlinear reduction step 2 */

    /* Multiply-based finalizer for strong finish */
    h1^=h1>>29;h1*=0x9E3779B97F4A7C15ULL;
    h1^=h1>>31;h1*=0x85EBCA77C2B2AE3DULL;
    h1^=h1>>37;h1*=0xBF58476D1CE4E5B9ULL;
    h1^=h1>>41;
    return h1;
}

/* ─── Reference implementations for comparison ─── */
#define PHI 0x9E3779B97F4A7C15ULL
#define M1 0x85EBCA77C2B2AE3DULL
#define M2 0xBF58476D1CE4E5B9ULL
#define M3 0x94D049BB133111EBULL

/* v2: multiply-based best quality */
uint64_t v2(const void*b,size_t l,uint64_t s){const uint8_t*p=b,*e=p+l;uint64_t ba=s^(l*PHI),h1=ba*PHI,h2=ba*M1,h3=ba*M2,h4=ba*M3;while(p+32<=e){h1^=rot(*(const uint64_t*)p,11);h1*=PHI;p+=8;h2^=rot(*(const uint64_t*)p,19);h2*=PHI;p+=8;h3^=rot(*(const uint64_t*)p,35);h3*=PHI;p+=8;h4^=rot(*(const uint64_t*)p,47);h4*=PHI;p+=8;}while(p+8<=e){h1^=rot(*(const uint64_t*)p,11);h1*=PHI;p+=8;}uint64_t t=0;switch(e-p){case 7:t^=(uint64_t)p[6]<<48;case 6:t^=(uint64_t)p[5]<<40;case 5:t^=(uint64_t)p[4]<<32;case 4:t^=(uint64_t)p[3]<<24;case 3:t^=(uint64_t)p[2]<<16;case 2:t^=(uint64_t)p[1]<<8;case 1:t^=p[0];h1^=rot(t,11);h1*=PHI;}h1^=h2;h3^=h4;h1^=h3;h1^=h1>>29;h1*=M1;h1^=h1>>31;h1*=M2;h1^=h1>>37;h1*=PHI;h1^=h1>>41;return h1;}

/* v3_arx: ARX-based */
uint64_t v3_arx(const void*b,size_t l,uint64_t s){const uint8_t*p=b,*e=p+l;uint64_t ba=s^(l*PHI),h1=ba*PHI,h2=ba*M1,h3=ba*M2,h4=ba*M3;while(p+32<=e){h1^=*(const uint64_t*)p;h1=rot(h1+h2,11);p+=8;h2^=*(const uint64_t*)p;h2=rot(h2+h3,17);p+=8;h3^=*(const uint64_t*)p;h3=rot(h3+h4,23);p+=8;h4^=*(const uint64_t*)p;h4=rot(h4+h1,57);p+=8;}while(p+8<=e){h1^=*(const uint64_t*)p;h1=rot(h1+h2,11);p+=8;}uint64_t t=0;switch(e-p){case 7:t^=(uint64_t)p[6]<<48;case 6:t^=(uint64_t)p[5]<<40;case 5:t^=(uint64_t)p[4]<<32;case 4:t^=(uint64_t)p[3]<<24;case 3:t^=(uint64_t)p[2]<<16;case 2:t^=(uint64_t)p[1]<<8;case 1:t^=p[0];h1^=t;h1=rot(h1+h2,11);}h1^=h2;h3^=h4;h1^=h3;h1^=h1>>29;h1*=M1;h1^=h1>>31;h1*=M2;h1^=h1>>37;h1*=PHI;h1^=h1>>41;return h1;}

int main(void){
    printf("=== ULTIMATE vs v2 vs ARX ===\n\n");
    uint8_t wb[1024];volatile uint64_t ws=0;
    for(int i=0;i<1000;i++){ws^=v2(wb,256,i);ws^=v3_arx(wb,256,i);ws^=ultimate(wb,256,i);}

    /* SPEED */
    printf("--- SPEED (GB/s) ---\n%8s","");size_t szs[]={48,64,256,1024,8192,65536,262144};int ns=7;
    struct{const char*n;void*f;}hs[]={{"v2(mul)",v2},{"v3_arx",v3_arx},{"ULTIMATE",ultimate}};int nh=3;
    for(int si=0;si<ns;si++)printf(" %8zu",szs[si]);printf("\n");
    for(int hi=0;hi<nh;hi++){printf("%-8s",hs[hi].n);typedef uint64_t(*hf)(const void*,size_t,uint64_t);hf fn=(hf)hs[hi].f;
        for(int si=0;si<ns;si++){size_t sz=szs[si];uint8_t*b=malloc(sz);for(size_t i=0;i<sz;i++)b[i]=i*0x9D+0x37;
            int it=sz<1024?50000000/sz:500000;if(it<10)it=10;volatile uint64_t s=0;clock_t st=clock();
            for(int i=0;i<it;i++)s^=fn(b,sz,i);clock_t en=clock();
            printf(" %8.1f",sz*(double)it/((double)(en-st)/CLOCKS_PER_SEC)/1e9);free(b);}printf(" GB/s\n");}

    /* QUALITY */
    printf("\n--- AVALANCHE (worst%%) & CHI2 ---\n");
    for(int hi=0;hi<nh;hi++){typedef uint64_t(*hf)(const void*,size_t,uint64_t);hf fn=(hf)hs[hi].f;
        uint8_t buf[256];for(int i=0;i<256;i++)buf[i]=i*0x9D+0x37;uint64_t base=fn(buf,256,0);double lo=100;
        for(int by=0;by<32;by++)for(int bi=0;bi<8;bi++){buf[by]^=(1u<<bi);uint64_t hv=fn(buf,256,0);buf[by]^=(1u<<bi);
            double p=pop(base^hv)/64.0*100;if(p<lo)lo=p;}
        int bins[256]={0};for(int i=0;i<256000;i++)bins[fn(&i,4,i)&255]++;
        double ex=1000.0,chi2=0;for(int b=0;b<256;b++){double d=bins[b]-ex;chi2+=d*d/ex;}
        int bc[64]={0};for(int i=0;i<100000;i++){uint64_t hv=fn(&i,4,i*0x9E3779B9);for(int b=0;b<64;b++)if(hv&(1ULL<<b))bc[b]++;}
        double wb=0;for(int b=0;b<64;b++){double bi=fabs(bc[b]/100000.0*100-50);if(bi>wb)wb=bi;}
        printf("%-8s ava=%.1f%% chi2=%.1f bias=%.2f%%\n",hs[hi].n,lo,chi2,wb+50);}
    return 0;
}
