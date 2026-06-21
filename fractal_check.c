#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#include <math.h>

static inline uint64_t rot(uint64_t x,int k){return(x<<k)|(x>>(64-k));}
static inline int pop(uint64_t x){x-=x>>1&0x5555555555555555ULL;x=(x&0x3333333333333333ULL)+(x>>2&0x3333333333333333ULL);x=(x+(x>>4))&0x0F0F0F0F0F0F0F0FULL;return(x*0x0101010101010101ULL)>>56;}
#define PHI 0x9E3779B97F4A7C15ULL
#define M1 0x85EBCA77C2B2AE3DULL
#define M2 0xBF58476D1CE4E5B9ULL
#define M3 0x94D049BB133111EBULL

/* Vortex baseline */
uint64_t vortex(const void*b,size_t l,uint64_t s){const uint8_t*p=b,*e=p+l;uint64_t ba=s^(l*PHI),h1=ba*PHI,h2=ba*M1,h3=ba*M2,h4=ba*M3;while(p+32<=e){h1^=*(const uint64_t*)p;h1=rot(h1+h2,11);p+=8;h2^=*(const uint64_t*)p;h2=rot(h2+h3,17);p+=8;h3^=*(const uint64_t*)p;h3=rot(h3+h4,23);p+=8;h4^=*(const uint64_t*)p;h4=rot(h4+h1,57);p+=8;}while(p+8<=e){h1^=*(const uint64_t*)p;h1=rot(h1+h2,11);p+=8;}uint64_t t=0;switch(e-p){case 7:t^=(uint64_t)p[6]<<48;case 6:t^=(uint64_t)p[5]<<40;case 5:t^=(uint64_t)p[4]<<32;case 4:t^=(uint64_t)p[3]<<24;case 3:t^=(uint64_t)p[2]<<16;case 2:t^=(uint64_t)p[1]<<8;case 1:t^=p[0];h1^=t;h1=rot(h1+h2,11);}h1^=h2;h3^=h4;h1^=h3;h1^=h1>>29;h1*=M1;h1^=h1>>31;h1*=M2;h1^=h1>>37;h1*=PHI;h1^=h1>>41;return h1;}

/* Fractal finalizer: ARX pattern repeated at finalization level */
uint64_t frac_fin(const void*b,size_t l,uint64_t s){const uint8_t*p=b,*e=p+l;uint64_t ba=s^(l*PHI),h1=ba*PHI,h2=ba*M1,h3=ba*M2,h4=ba*M3;while(p+32<=e){h1^=*(const uint64_t*)p;h1=rot(h1+h2,11);p+=8;h2^=*(const uint64_t*)p;h2=rot(h2+h3,17);p+=8;h3^=*(const uint64_t*)p;h3=rot(h3+h4,23);p+=8;h4^=*(const uint64_t*)p;h4=rot(h4+h1,57);p+=8;}while(p+8<=e){h1^=*(const uint64_t*)p;h1=rot(h1+h2,11);p+=8;}uint64_t t=0;switch(e-p){case 7:t^=(uint64_t)p[6]<<48;case 6:t^=(uint64_t)p[5]<<40;case 5:t^=(uint64_t)p[4]<<32;case 4:t^=(uint64_t)p[3]<<24;case 3:t^=(uint64_t)p[2]<<16;case 2:t^=(uint64_t)p[1]<<8;case 1:t^=p[0];h1^=t;h1=rot(h1+h2,11);}h1=rot(h1+h2,11);h2=rot(h2+h3,17);h3=rot(h3+h4,23);h4=rot(h4+h1,57);h1=rot(h1+h3,13);h2=rot(h2+h4,31);h1=rot(h1+h2,19);h1^=h2;h3^=h4;h1^=h3;h1^=h1>>29;h1*=M1;h1^=h1>>31;h1*=M2;h1^=h1>>37;h1*=PHI;h1^=h1>>41;return h1;}

/* Julia: z=z^2+c using 128-bit multiply */
static inline uint64_t jmix(uint64_t z,uint64_t c){__uint128_t sq=(__uint128_t)z*z;z=(uint64_t)sq^(uint64_t)(sq>>64);z^=c;return rot(z,17);}
uint64_t julia(const void*b,size_t l,uint64_t s){const uint8_t*p=b,*e=p+l;uint64_t h1=s^PHI,h2=s^M1,h3=s^M2,h4=s^(l*PHI);while(p+32<=e){h1=jmix(h1^*(const uint64_t*)p,PHI);p+=8;h2=jmix(h2^*(const uint64_t*)p,M1);p+=8;h3=jmix(h3^*(const uint64_t*)p,M2);p+=8;h4=jmix(h4^*(const uint64_t*)p,PHI);p+=8;}while(p+8<=e){h1=jmix(h1^*(const uint64_t*)p,PHI);p+=8;}uint64_t t=0;switch(e-p){case 7:t^=(uint64_t)p[6]<<48;case 6:t^=(uint64_t)p[5]<<40;case 5:t^=(uint64_t)p[4]<<32;case 4:t^=(uint64_t)p[3]<<24;case 3:t^=(uint64_t)p[2]<<16;case 2:t^=(uint64_t)p[1]<<8;case 1:t^=p[0];h1=jmix(h1^t,PHI);}h1^=h2;h3^=h4;h1^=h3;h1=jmix(h1,PHI);h1=jmix(h1,M1);h1=jmix(h1,M2);h1=jmix(h1,PHI);return h1;}

typedef uint64_t(*hf)(const void*,size_t,uint64_t);

double bench(hf fn,size_t sz){
    uint8_t*b=malloc(sz);for(size_t i=0;i<sz;i++)b[i]=i*0x9D+0x37;
    int it=sz<1024?10000000/sz:100000;if(it<10)it=10;
    volatile uint64_t s=0;clock_t st=clock();
    for(int i=0;i<it;i++)s^=fn(b,sz,i);clock_t en=clock();
    free(b);return sz*(double)it/((double)(en-st)/CLOCKS_PER_SEC)/1e9;
}

int main(void){
    printf("=== Fractal Hash Check ===\n\n");
    struct{const char*n;hf f;}hs[]={{"Vortex",vortex},{"FracFin",frac_fin},{"Julia",julia}};
    int nh=3;

    /* warmup */
    uint8_t wb[256];volatile uint64_t ws=0;
    for(int i=0;i<500;i++)for(int j=0;j<nh;j++)ws^=hs[j].f(wb,256,i);

    /* Correctness: all should produce different outputs for same input */
    printf("Correctness (256B, seed=42):\n");
    for(int i=0;i<nh;i++)printf("  %-8s = %016llx\n",hs[i].n,(unsigned long long)hs[i].f(wb,256,42));
    printf("  All different: %s\n\n",
        hs[0].f(wb,256,42)!=hs[1].f(wb,256,42)&&hs[0].f(wb,256,42)!=hs[2].f(wb,256,42)&&hs[1].f(wb,256,42)!=hs[2].f(wb,256,42)?"YES":"NO");

    /* Speed */
    printf("Speed (GB/s):\n%8s","");size_t szs[]={64,256,1024,8192};int ns=4;
    for(int si=0;si<ns;si++)printf(" %8zu",szs[si]);printf("\n");
    for(int hi=0;hi<nh;hi++){
        printf("%-8s",hs[hi].n);
        for(int si=0;si<ns;si++)printf(" %8.1f",bench(hs[hi].f,szs[si]));
        printf(" GB/s\n");
    }

    /* Quality */
    printf("\nQuality:\n%8s %6s %7s\n","","ava%","chi2");
    for(int hi=0;hi<nh;hi++){
        hf fn=hs[hi].f;
        uint8_t buf[256];for(int i=0;i<256;i++)buf[i]=i*0x9D+0x37;
        uint64_t base=fn(buf,256,0);double lo=100;
        for(int by=0;by<32;by++)for(int bi=0;bi<8;bi++){buf[by]^=(1u<<bi);uint64_t hv=fn(buf,256,0);buf[by]^=(1u<<bi);double p=pop(base^hv)/64.0*100;if(p<lo)lo=p;}
        int bins[256]={0};for(int i=0;i<256000;i++)bins[fn(&i,4,i)&255]++;
        double ex=1000.0,chi2=0;for(int b=0;b<256;b++){double d=bins[b]-ex;chi2+=d*d/ex;}
        printf("%-8s %5.1f%% %7.1f\n",hs[hi].n,lo,chi2);
    }
    return 0;
}
