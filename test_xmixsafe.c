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

/* Fast path: pure ARX, no cross-mix */
uint64_t fast(const void*b,size_t l,uint64_t s){const uint8_t*p=b,*e=p+l;uint64_t ba=s^(l*VP),h1=ba*VP,h2=ba*VM1,h3=ba*VM2,h4=ba*VM3;while(p+32<=e){h1^=*(const uint64_t*)p;h1=rot(h1+h2,11);p+=8;h2^=*(const uint64_t*)p;h2=rot(h2+h3,17);p+=8;h3^=*(const uint64_t*)p;h3=rot(h3+h4,23);p+=8;h4^=*(const uint64_t*)p;h4=rot(h4+h1,57);p+=8;}while(p+8<=e){h1^=*(const uint64_t*)p;h1=rot(h1+h2,11);p+=8;}uint64_t t=0;switch(e-p){case 7:t^=(uint64_t)p[6]<<48;case 6:t^=(uint64_t)p[5]<<40;case 5:t^=(uint64_t)p[4]<<32;case 4:t^=(uint64_t)p[3]<<24;case 3:t^=(uint64_t)p[2]<<16;case 2:t^=(uint64_t)p[1]<<8;case 1:t^=p[0];h1^=t;h1=rot(h1+h2,11);}h1^=h2;h3^=h4;h1^=h3;h1^=h1>>29;h1*=VM1;h1^=h1>>31;h1*=VM2;h1^=h1>>37;h1*=VP;h1^=h1>>41;return h1;}

/* Safe with cross-mix xr=43 */
uint64_t safe_xm(const void*b,size_t l,uint64_t s){const uint8_t*p=b,*e=p+l;uint64_t ba=s^(l*VP),h1=ba*VP,h2=ba*VM1,h3=ba*VM2,h4=ba*VM3,acc=ba^VM2;int hb=0;
    while(p+32<=e){uint64_t v1=*(const uint64_t*)p;p+=8;uint64_t v2=*(const uint64_t*)p;p+=8;uint64_t v3=*(const uint64_t*)p;p+=8;uint64_t v4=*(const uint64_t*)p;p+=8;
        h1^=v1;h1=rot(h1+h2,11);h2^=v2;h2=rot(h2+h3,17);h3^=v3;h3=rot(h3+h4,23);h4^=v4;h4=rot(h4+h1,57);acc^=v1^v2^v3^v4;acc=rot(acc,31);acc*=VP;hb=1;}
    while(p+8<=e){uint64_t v=*(const uint64_t*)p;p+=8;h1^=v;h1=rot(h1+h2,11);acc^=v;acc=rot(acc,31);acc*=VP;hb=1;}
    /* Cross-mix at xr=43 */
    h2^=h1;h2=rot(h2,43);h1^=h2;
    uint64_t t=0;switch(e-p){case 7:t^=(uint64_t)p[6]<<48;case 6:t^=(uint64_t)p[5]<<40;case 5:t^=(uint64_t)p[4]<<32;case 4:t^=(uint64_t)p[3]<<24;case 3:t^=(uint64_t)p[2]<<16;case 2:t^=(uint64_t)p[1]<<8;case 1:t^=p[0];}if(t){h1^=t;h1=rot(h1+h2,11);if(hb){acc^=t;acc=rot(acc,31);acc*=VP;}}
    h1^=h2;h3^=h4;h1^=h3;if(hb)h1^=acc;h1^=h1>>29;h1*=VM1;h1^=h1>>31;h1*=VM2;h1^=h1>>37;h1*=VP;h1^=h1>>41;return h1;}

/* Original safe for reference */
#define PLUMHASH_IMPLEMENTATION
#include "plumhash.h"

int main(void){
    printf("=== Cross-mix xr=43 in SAFE path only ===\n\n");
    struct{const char*n;hf f;} hs[]={{"ORIGINAL",plumhash},{"XMIX43",safe_xm},{"FAST",fast}}; int nh=3;
    uint8_t wb[256]; volatile uint64_t ws=0;
    for(int i=0;i<500;i++) for(int j=0;j<nh;j++) ws^=hs[j].f(wb,256,i);
    printf("%-10s %6s %7s %6s\n","","ava%","chi2","sparse");
    for(int hi=0;hi<nh;hi++) { hf fn=hs[hi].f;
        /* ava on 256B */
        uint8_t buf[256]; for(int i=0;i<256;i++) buf[i]=i*0x9D+0x37; uint64_t base=fn(buf,256,0); double lo=100;
        for(int by=0;by<32;by++) for(int bi=0;bi<8;bi++) { buf[by]^=(1u<<bi); uint64_t hv=fn(buf,256,0); buf[by]^=(1u<<bi); double p=pop(base^hv)/64.0*100; if(p<lo) lo=p; }
        /* chi2 on 4B */
        int bins[256]={0}; for(int i=0;i<256000;i++) bins[fn(&i,4,i)&255]++; double ex=1000.0,chi2=0; for(int b=0;b<256;b++){double d=bins[b]-ex;chi2+=d*d/ex;}
        /* sparse */
        int cols=0; uint64_t seen[20000]={0}; int ngen=0; uint8_t key[256]; for(int pos=0;pos<128&&ngen<20000;pos++) for(int val=1;val<256&&ngen<20000;val++) { int kl=8+(pos%57); memset(key,0,kl); key[kl-1]=val; if(pos&1) key[0]=pos^val; seen[ngen++]=fn(key,kl,0); } for(int i=0;i<ngen;i++) for(int j=i+1;j<ngen;j++) if(seen[i]==seen[j]) cols++;
        printf("%-10s %5.1f%% %7.1f %5d\n",hs[hi].n,lo,chi2,cols);
    }
    return 0;
}
