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
typedef uint64_t(*hf)(const void*,size_t,uint64_t);

/* Vortex baseline */
uint64_t base(const void*b,size_t l,uint64_t s){const uint8_t*p=b,*e=p+l;uint64_t ba=s^(l*PHI),h1=ba*PHI,h2=ba*M1,h3=ba*M2,h4=ba*M3;while(p+32<=e){h1^=*(const uint64_t*)p;h1=rot(h1+h2,11);p+=8;h2^=*(const uint64_t*)p;h2=rot(h2+h3,17);p+=8;h3^=*(const uint64_t*)p;h3=rot(h3+h4,23);p+=8;h4^=*(const uint64_t*)p;h4=rot(h4+h1,57);p+=8;}while(p+8<=e){h1^=*(const uint64_t*)p;h1=rot(h1+h2,11);p+=8;}uint64_t t=0;switch(e-p){case 7:t^=(uint64_t)p[6]<<48;case 6:t^=(uint64_t)p[5]<<40;case 5:t^=(uint64_t)p[4]<<32;case 4:t^=(uint64_t)p[3]<<24;case 3:t^=(uint64_t)p[2]<<16;case 2:t^=(uint64_t)p[1]<<8;case 1:t^=p[0];h1^=t;h1=rot(h1+h2,11);}h1^=h2;h3^=h4;h1^=h3;h1^=h1>>29;h1*=M1;h1^=h1>>31;h1*=M2;h1^=h1>>37;h1*=PHI;h1^=h1>>41;return h1;}

/* Bracket: forward multiply → ARX → reverse multiply (modular inverse) */
/* PHI inverse mod 2^64 = 0x9E3779B97F4A7C15 is its own inverse? No. */
/* Actually for any odd C, C^(-1) mod 2^64 exists. Let me use a pair. */
#define FWD 0xBF58476D1CE4E5B9ULL  /* M2 */
#define REV 0x94D049BB133111EBULL  /* M3 — NOT the inverse, just a different prime */
/* Key idea: FWD and REV are DIFFERENT primes, so forward*reverse ≠ 1 */
/* This creates an asymmetric bracket that doesn't cancel completely */

uint64_t bracket(const void*b,size_t l,uint64_t s){const uint8_t*p=b,*e=p+l;uint64_t ba=s^(l*PHI),h1=ba*PHI,h2=ba*M1,h3=ba*M2,h4=ba*M3;
    while(p+32<=e){
        /* Forward spread with FWD prime */
        h1*=FWD;h2*=FWD;h3*=FWD;h4*=FWD;
        /* ARX core */
        h1^=*(const uint64_t*)p;h1=rot(h1+h2,11);p+=8;h2^=*(const uint64_t*)p;h2=rot(h2+h3,17);p+=8;h3^=*(const uint64_t*)p;h3=rot(h3+h4,23);p+=8;h4^=*(const uint64_t*)p;h4=rot(h4+h1,57);p+=8;
        /* Reverse fold with REV prime (different from FWD) */
        h1*=REV;h2*=REV;h3*=REV;h4*=REV;
    }
    while(p+8<=e){h1*=FWD;h1^=*(const uint64_t*)p;h1=rot(h1+h2,11);p+=8;h1*=REV;}
    uint64_t t=0;switch(e-p){case 7:t^=(uint64_t)p[6]<<48;case 6:t^=(uint64_t)p[5]<<40;case 5:t^=(uint64_t)p[4]<<32;case 4:t^=(uint64_t)p[3]<<24;case 3:t^=(uint64_t)p[2]<<16;case 2:t^=(uint64_t)p[1]<<8;case 1:t^=p[0];h1*=FWD;h1^=t;h1=rot(h1+h2,11);h1*=REV;}
    h1^=h2;h3^=h4;h1^=h3;h1^=h1>>29;h1*=M1;h1^=h1>>31;h1*=M2;h1^=h1>>37;h1*=PHI;h1^=h1>>41;return h1;
}

/* Bracket lite: only bracket h1 (fast lane), others normal */
uint64_t bracket2(const void*b,size_t l,uint64_t s){const uint8_t*p=b,*e=p+l;uint64_t ba=s^(l*PHI),h1=ba*PHI,h2=ba*M1,h3=ba*M2,h4=ba*M3;
    while(p+32<=e){
        h1*=FWD;h1^=*(const uint64_t*)p;h1=rot(h1+h2,11);p+=8;h1*=REV;
        h2^=*(const uint64_t*)p;h2=rot(h2+h3,17);p+=8;
        h3^=*(const uint64_t*)p;h3=rot(h3+h4,23);p+=8;
        h4^=*(const uint64_t*)p;h4=rot(h4+h1,57);p+=8;
    }
    while(p+8<=e){h1*=FWD;h1^=*(const uint64_t*)p;h1=rot(h1+h2,11);p+=8;h1*=REV;}
    uint64_t t=0;switch(e-p){case 7:t^=(uint64_t)p[6]<<48;case 6:t^=(uint64_t)p[5]<<40;case 5:t^=(uint64_t)p[4]<<32;case 4:t^=(uint64_t)p[3]<<24;case 3:t^=(uint64_t)p[2]<<16;case 2:t^=(uint64_t)p[1]<<8;case 1:t^=p[0];h1^=t;h1=rot(h1+h2,11);}
    h1^=h2;h3^=h4;h1^=h3;h1^=h1>>29;h1*=M1;h1^=h1>>31;h1*=M2;h1^=h1>>37;h1*=PHI;h1^=h1>>41;return h1;
}

int main(void){printf("=== Bracket Primes ===\n\n");
    uint8_t wb[256];volatile uint64_t ws=0;for(int i=0;i<500;i++){ws^=base(wb,256,i);ws^=bracket(wb,256,i);ws^=bracket2(wb,256,i);}
    struct{const char*n;hf f;}hs[]={{"Base",base},{"Bracket",bracket},{"Bracket2",bracket2}};int nh=3;
    printf("Speed: %8s","");size_t szs[]={64,256,1024,65536};for(int si=0;si<4;si++)printf(" %8zu",szs[si]);printf("\n");
    for(int hi=0;hi<nh;hi++){printf("%-10s",hs[hi].n);
        for(int si=0;si<4;si++){size_t sz=szs[si];uint8_t*b=malloc(sz);for(size_t i=0;i<sz;i++)b[i]=i*0x9D+0x37;
            int it=sz<1024?20000000/sz:200000;volatile uint64_t s=0;clock_t st=clock();for(int i=0;i<it;i++)s^=hs[hi].f(b,sz,i);clock_t en=clock();
            printf(" %8.1f",sz*(double)it/((double)(en-st)/CLOCKS_PER_SEC)/1e9);free(b);}printf(" GB/s\n");}
    printf("\nQuality: %8s %6s %7s\n","","ava%","chi2");
    for(int hi=0;hi<nh;hi++){uint8_t buf[256];for(int i=0;i<256;i++)buf[i]=i*0x9D+0x37;uint64_t bv=hs[hi].f(buf,256,0);double lo=100;
        for(int by=0;by<32;by++)for(int bi=0;bi<8;bi++){buf[by]^=(1u<<bi);uint64_t hv=hs[hi].f(buf,256,0);buf[by]^=(1u<<bi);double p=pop(bv^hv)/64.0*100;if(p<lo)lo=p;}
        int bins[256]={0};for(int i=0;i<256000;i++)bins[hs[hi].f(&i,4,i)&255]++;double ex=1000.0,chi2=0;for(int b=0;b<256;b++){double d=bins[b]-ex;chi2+=d*d/ex;}
        printf("%-10s %5.1f%% %7.1f\n",hs[hi].n,lo,chi2);}
    return 0;
}
