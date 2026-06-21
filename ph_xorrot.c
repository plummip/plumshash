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

/* ARX baseline */
uint64_t arx(const void*b,size_t l,uint64_t s){const uint8_t*p=b,*e=p+l;uint64_t ba=s^(l*VP),h1=ba*VP,h2=ba*VM1,h3=ba*VM2,h4=ba*VM3;while(p+32<=e){h1^=*(const uint64_t*)p;h1=rot(h1+h2,11);p+=8;h2^=*(const uint64_t*)p;h2=rot(h2+h3,17);p+=8;h3^=*(const uint64_t*)p;h3=rot(h3+h4,23);p+=8;h4^=*(const uint64_t*)p;h4=rot(h4+h1,57);p+=8;}while(p+8<=e){h1^=*(const uint64_t*)p;h1=rot(h1+h2,11);p+=8;}uint64_t t=0;switch(e-p){case 7:t^=(uint64_t)p[6]<<48;case 6:t^=(uint64_t)p[5]<<40;case 5:t^=(uint64_t)p[4]<<32;case 4:t^=(uint64_t)p[3]<<24;case 3:t^=(uint64_t)p[2]<<16;case 2:t^=(uint64_t)p[1]<<8;case 1:t^=p[0];h1^=t;h1=rot(h1+h2,11);}h1^=h2;h3^=h4;h1^=h3;h1^=h1>>29;h1*=VM1;h1^=h1>>31;h1*=VM2;h1^=h1>>37;h1*=VP;h1^=h1>>41;return h1;}

/* XOR+rotate only (no add), stronger finalizer */
uint64_t xr(const void*b,size_t l,uint64_t s){const uint8_t*p=b,*e=p+l;uint64_t ba=s^(l*VP),h1=ba*VP,h2=ba*VM1,h3=ba*VM2,h4=ba*VM3;
    while(p+32<=e){
        h1^=*(const uint64_t*)p;h1=rot(h1,11);p+=8;h2^=*(const uint64_t*)p;h2=rot(h2,17);p+=8;
        h3^=*(const uint64_t*)p;h3=rot(h3,23);p+=8;h4^=*(const uint64_t*)p;h4=rot(h4,57);p+=8;
    }
    while(p+8<=e){h1^=*(const uint64_t*)p;h1=rot(h1,11);p+=8;}
    uint64_t t=0;switch(e-p){case 7:t^=(uint64_t)p[6]<<48;case 6:t^=(uint64_t)p[5]<<40;case 5:t^=(uint64_t)p[4]<<32;case 4:t^=(uint64_t)p[3]<<24;case 3:t^=(uint64_t)p[2]<<16;case 2:t^=(uint64_t)p[1]<<8;case 1:t^=p[0];h1^=t;h1=rot(h1,11);}
    h1^=h2;h3^=h4;h1^=h3;
    /* Stronger finalizer: 6 rounds to compensate for no-add body */
    h1^=h1>>17;h1*=VM1;h1^=h1>>23;h1*=VM2;h1^=h1>>29;h1*=VM3;h1^=h1>>37;h1*=VP;h1^=h1>>43;h1*=VM1;h1^=h1>>53;
    return h1;
}

/* XOR+rotate with interleaved lane mixing every 2 blocks */
uint64_t xr2(const void*b,size_t l,uint64_t s){const uint8_t*p=b,*e=p+l;uint64_t ba=s^(l*VP),h1=ba*VP,h2=ba*VM1,h3=ba*VM2,h4=ba*VM3;int blk=0;
    while(p+32<=e){
        h1^=*(const uint64_t*)p;h1=rot(h1,11);p+=8;h2^=*(const uint64_t*)p;h2=rot(h2,17);p+=8;
        h3^=*(const uint64_t*)p;h3=rot(h3,23);p+=8;h4^=*(const uint64_t*)p;h4=rot(h4,57);p+=8;
        if(++blk%2==0){uint64_t tmp=h1;h1^=rot(h3,31);h3^=rot(tmp,37);tmp=h2;h2^=rot(h4,43);h4^=rot(tmp,53);}
    }
    while(p+8<=e){h1^=*(const uint64_t*)p;h1=rot(h1,11);p+=8;}
    uint64_t t=0;switch(e-p){case 7:t^=(uint64_t)p[6]<<48;case 6:t^=(uint64_t)p[5]<<40;case 5:t^=(uint64_t)p[4]<<32;case 4:t^=(uint64_t)p[3]<<24;case 3:t^=(uint64_t)p[2]<<16;case 2:t^=(uint64_t)p[1]<<8;case 1:t^=p[0];h1^=t;h1=rot(h1,11);}
    h1^=h2;h3^=h4;h1^=h3;h1^=h1>>29;h1*=VM1;h1^=h1>>31;h1*=VM2;h1^=h1>>37;h1*=VP;h1^=h1>>41;return h1;
}

int main(void){printf("=== ARX vs XOR+Rotate ===\n\n");
    uint8_t wb[256];volatile uint64_t ws=0;for(int i=0;i<500;i++){ws^=arx(wb,256,i);ws^=xr(wb,256,i);ws^=xr2(wb,256,i);}
    struct{const char*n;hf f;}hs[]={{"ARX",arx},{"XR",xr},{"XR2",xr2}};int nh=3;
    printf("Speed (GB/s):\n%6s","");size_t szs[]={64,256,1024,65536};for(int si=0;si<4;si++)printf(" %8zu",szs[si]);printf("\n");
    for(int hi=0;hi<nh;hi++){printf("%-6s",hs[hi].n);
        for(int si=0;si<4;si++){size_t sz=szs[si];uint8_t*b=malloc(sz);for(size_t i=0;i<sz;i++)b[i]=i*0x9D+0x37;
            int it=sz<1024?20000000/sz:200000;volatile uint64_t s=0;clock_t st=clock();for(int i=0;i<it;i++)s^=hs[hi].f(b,sz,i);clock_t en=clock();
            printf(" %8.1f",sz*(double)it/((double)(en-st)/CLOCKS_PER_SEC)/1e9);free(b);}printf(" GB/s\n");}
    printf("\nQuality:\n%6s %6s %7s\n","","ava%","chi2");
    for(int hi=0;hi<nh;hi++){uint8_t buf[256];for(int i=0;i<256;i++)buf[i]=i*0x9D+0x37;uint64_t base=hs[hi].f(buf,256,0);double lo=100;
        for(int by=0;by<32;by++)for(int bi=0;bi<8;bi++){buf[by]^=(1u<<bi);uint64_t hv=hs[hi].f(buf,256,0);buf[by]^=(1u<<bi);double p=pop(base^hv)/64.0*100;if(p<lo)lo=p;}
        int bins[256]={0};for(int i=0;i<256000;i++)bins[hs[hi].f(&i,4,i)&255]++;double ex=1000.0,chi2=0;for(int b=0;b<256;b++){double d=bins[b]-ex;chi2+=d*d/ex;}
        printf("%-6s %5.1f%% %7.1f\n",hs[hi].n,lo,chi2);}
    return 0;
}
