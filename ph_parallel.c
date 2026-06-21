#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#include <math.h>
static inline uint64_t rot(uint64_t x,int k){return(x<<k)|(x>>(64-k));}
static inline int pop(uint64_t x){x-=x>>1&0x5555555555555555ULL;x=(x&0x3333333333333333ULL)+(x>>2&0x3333333333333333ULL);x=(x+(x>>4))&0x0F0F0F0F0F0F0F0FULL;return(x*0x0101010101010101ULL)>>56;}
#define VP 0x9E3779B97F4A7C15ULL
#define VM1 0x85EBCA77C2B2AE3DULL
#define VM2 0xBF58476D1CE4E5B9ULL
#define VM3 0x94D049BB133111EBULL
typedef uint64_t(*hf)(const void*,size_t,uint64_t);

/* Current: feedback chain (h4 uses new h1) */
uint64_t cur(const void*b,size_t l,uint64_t s){const uint8_t*p=b,*e=p+l;uint64_t ba=s^(l*VP),h1=ba*VP,h2=ba*VM1,h3=ba*VM2,h4=ba*VM3;while(p+32<=e){h1^=*(const uint64_t*)p;p+=8;h1=rot(h1+h2,11);h2^=*(const uint64_t*)p;p+=8;h2=rot(h2+h3,17);h3^=*(const uint64_t*)p;p+=8;h3=rot(h3+h4,23);h4^=*(const uint64_t*)p;p+=8;h4=rot(h4+h1,57);}while(p+8<=e){h1^=*(const uint64_t*)p;h1=rot(h1+h2,11);p+=8;}uint64_t t=0;switch(e-p){case 7:t^=(uint64_t)p[6]<<48;case 6:t^=(uint64_t)p[5]<<40;case 5:t^=(uint64_t)p[4]<<32;case 4:t^=(uint64_t)p[3]<<24;case 3:t^=(uint64_t)p[2]<<16;case 2:t^=(uint64_t)p[1]<<8;case 1:t^=p[0];h1^=t;h1=rot(h1+h2,11);}h1^=h2;h3^=h4;h1^=h3;h1^=h1>>29;h1*=VM1;h1^=h1>>31;h1*=VM2;h1^=h1>>37;h1*=VP;h1^=h1>>41;return h1;}

/* Parallel: no feedback (h4 uses OLD h1), then cross-mix after each block */
uint64_t par(const void*b,size_t l,uint64_t s){const uint8_t*p=b,*e=p+l;uint64_t ba=s^(l*VP),h1=ba*VP,h2=ba*VM1,h3=ba*VM2,h4=ba*VM3;while(p+32<=e){
    uint64_t v1=*(const uint64_t*)p;p+=8;uint64_t v2=*(const uint64_t*)p;p+=8;
    uint64_t v3=*(const uint64_t*)p;p+=8;uint64_t v4=*(const uint64_t*)p;p+=8;
    uint64_t old_h1=h1; /* snapshot for parallel execution */
    h1=rot((h1^v1)+h2,11);h2=rot((h2^v2)+h3,17);h3=rot((h3^v3)+h4,23);h4=rot((h4^v4)+old_h1,57);
    /* Cross-mix to compensate for broken feedback */
    h1^=rot(h4,31);h2^=rot(h1,37);h3^=rot(h2,43);h4^=rot(h3,53);
}while(p+8<=e){h1^=*(const uint64_t*)p;h1=rot(h1+h2,11);p+=8;}uint64_t t=0;switch(e-p){case 7:t^=(uint64_t)p[6]<<48;case 6:t^=(uint64_t)p[5]<<40;case 5:t^=(uint64_t)p[4]<<32;case 4:t^=(uint64_t)p[3]<<24;case 3:t^=(uint64_t)p[2]<<16;case 2:t^=(uint64_t)p[1]<<8;case 1:t^=p[0];h1^=t;h1=rot(h1+h2,11);}h1^=h2;h3^=h4;h1^=h3;h1^=h1>>29;h1*=VM1;h1^=h1>>31;h1*=VM2;h1^=h1>>37;h1*=VP;h1^=h1>>41;return h1;}

int main(void){printf("=== Feedback vs Parallel ===\n\n");
    uint8_t wb[256];volatile uint64_t ws=0;for(int i=0;i<500;i++){ws^=cur(wb,256,i);ws^=par(wb,256,i);}
    struct{const char*n;hf f;}hs[]={{"Feedback",cur},{"Parallel",par}};int nh=2;
    printf("Speed (GB/s):\n%10s","");size_t szs[]={64,256,1024,65536,262144};for(int si=0;si<5;si++)printf(" %8zu",szs[si]);printf("\n");
    for(int hi=0;hi<nh;hi++){printf("%-10s",hs[hi].n);
        for(int si=0;si<5;si++){size_t sz=szs[si];uint8_t*b=malloc(sz);for(size_t i=0;i<sz;i++)b[i]=i*0x9D+0x37;
            int it=sz<1024?30000000/sz:300000;volatile uint64_t s=0;clock_t st=clock();for(int i=0;i<it;i++)s^=hs[hi].f(b,sz,i);clock_t en=clock();
            printf(" %8.1f",sz*(double)it/((double)(en-st)/CLOCKS_PER_SEC)/1e9);free(b);}printf(" GB/s\n");}
    printf("\nQuality:\n%10s %6s %7s\n","","ava%","chi2");
    for(int hi=0;hi<nh;hi++){uint8_t buf[256];for(int i=0;i<256;i++)buf[i]=i*0x9D+0x37;uint64_t base=hs[hi].f(buf,256,0);double lo=100;
        for(int by=0;by<32;by++)for(int bi=0;bi<8;bi++){buf[by]^=(1u<<bi);uint64_t hv=hs[hi].f(buf,256,0);buf[by]^=(1u<<bi);double p=pop(base^hv)/64.0*100;if(p<lo)lo=p;}
        int bins[256]={0};for(int i=0;i<256000;i++)bins[hs[hi].f(&i,4,i)&255]++;double ex=1000.0,chi2=0;for(int b=0;b<256;b++){double d=bins[b]-ex;chi2+=d*d/ex;}
        printf("%-10s %5.1f%% %7.1f\n",hs[hi].n,lo,chi2);}
    return 0;
}
