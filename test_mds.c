#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#include <math.h>
static inline uint64_t rot(uint64_t x,int k){return(x<<k)|(x>>(64-k));}
static inline int pop(uint64_t x){x-=x>>1&0x5555555555555555ULL;x=(x&0x3333333333333333ULL)+(x>>2&0x3333333333333333ULL);x=(x+(x>>4))&0x0F0F0F0F0F0F0F0FULL;return(x*0x0101010101010101ULL)>>56;}
#define P 0x9E3779B97F4A7C15ULL
#define M1 0x85EBCA77C2B2AE3DULL
#define M2 0xBF58476D1CE4E5B9ULL
#define M3 0x94D049BB133111EBULL
typedef uint64_t(*hf)(const void*,size_t,uint64_t);

/* GF(2^64) multiply by X: shift left 1, conditional XOR with reduction polynomial */
static inline uint64_t gf_mul_x(uint64_t x) {
    return (x << 1) ^ ((int64_t)x >> 63 ? 0x1BULL : 0);  /* 0x1B = minimal poly for GF(2^64)? Actually for 64-bit we need the proper irreducible poly */
}
/* Actually for 64-bit, irreducible poly could be x^64 + x^4 + x^3 + x + 1 (like CRC-64) */
static inline uint64_t gf_mulx(uint64_t x) {
    return (x << 1) ^ ((int64_t)x >> 63 ? 0x1B00000000000000ULL : 0); /* top bit poly */
}
/* Simpler: just use rotate+XOR which is MDS-like without full GF */
static inline void mds_mix(uint64_t *a,uint64_t *b,uint64_t *c,uint64_t *d){
    /* Saturnin-inspired MDS: rotate + XOR chain */
    uint64_t ta=*a,tb=*b,tc=*c,td=*d;
    *a = ta ^ rot(tb,13) ^ rot(tc,29) ^ rot(td,43);
    *b = rot(ta,17) ^ tb ^ rot(tc,31) ^ rot(td,47);
    *c = rot(ta,23) ^ rot(tb,37) ^ tc ^ rot(td,53);
    *d = rot(ta,11) ^ rot(tb,19) ^ rot(tc,33) ^ td;
}

/* MDS-based finalizer (no multiply) */
uint64_t f_mds(const void*b,size_t l,uint64_t s){const uint8_t*p=b,*e=p+l;uint64_t ba=s^(l*P),h1=ba*P,h2=ba*M1,h3=ba*M2,h4=ba*M3;while(p+32<=e){h1^=*(const uint64_t*)p;p+=8;h1=rot(h1+h2,11);h2^=*(const uint64_t*)p;p+=8;h2=rot(h2+h3,17);h3^=*(const uint64_t*)p;p+=8;h3=rot(h3+h4,23);h4^=*(const uint64_t*)p;p+=8;h4=rot(h4+h1,57);}while(p+8<=e){h1^=*(const uint64_t*)p;p+=8;h1=rot(h1+h2,11);}uint64_t t=0;switch(e-p){case 7:t^=(uint64_t)p[6]<<48;case 6:t^=(uint64_t)p[5]<<40;case 5:t^=(uint64_t)p[4]<<32;case 4:t^=(uint64_t)p[3]<<24;case 3:t^=(uint64_t)p[2]<<16;case 2:t^=(uint64_t)p[1]<<8;case 1:t^=p[0];h1^=t;h1=rot(h1+h2,11);}
    /* MDS mix instead of XOR-only lane compression */
    mds_mix(&h1,&h2,&h3,&h4);
    h1^=h2;h3^=h4;h1^=h3;
    /* MDS rounds as finalizer */
    mds_mix(&h1,&h1,&h1,&h1);  /* self-mix */
    h1^=h1>>29;h1=rot(h1+h1,17);h1^=h1>>31;h1=rot(h1+h1,23);h1^=h1>>37;h1=rot(h1+h1,29);h1^=h1>>41;
    return h1;
}

/* Baseline */
uint64_t f_base(const void*b,size_t l,uint64_t s){const uint8_t*p=b,*e=p+l;uint64_t ba=s^(l*P),h1=ba*P,h2=ba*M1,h3=ba*M2,h4=ba*M3;while(p+32<=e){h1^=*(const uint64_t*)p;p+=8;h1=rot(h1+h2,11);h2^=*(const uint64_t*)p;p+=8;h2=rot(h2+h3,17);h3^=*(const uint64_t*)p;p+=8;h3=rot(h3+h4,23);h4^=*(const uint64_t*)p;p+=8;h4=rot(h4+h1,57);}while(p+8<=e){h1^=*(const uint64_t*)p;p+=8;h1=rot(h1+h2,11);}uint64_t t=0;switch(e-p){case 7:t^=(uint64_t)p[6]<<48;case 6:t^=(uint64_t)p[5]<<40;case 5:t^=(uint64_t)p[4]<<32;case 4:t^=(uint64_t)p[3]<<24;case 3:t^=(uint64_t)p[2]<<16;case 2:t^=(uint64_t)p[1]<<8;case 1:t^=p[0];h1^=t;h1=rot(h1+h2,11);}h1^=h2;h3^=h4;h1^=h3;h1^=h1>>29;h1*=M1;h1^=h1>>31;h1*=M2;h1^=h1>>37;h1*=P;h1^=h1>>41;return h1;}

int main(void){
    printf("=== MDS Finalizer ===\n\n");
    struct{const char*n;hf f;} hs[]={{"Base",f_base},{"MDS",f_mds}}; int nh=2;
    uint8_t wb[256]; volatile uint64_t ws=0; for(int i=0;i<500;i++) for(int j=0;j<nh;j++) ws^=hs[j].f(wb,256,i);
    printf("%-6s %8s %8s %6s %7s\n","","64B","256B","ava%","chi2");
    for(int hi=0;hi<nh;hi++){printf("%-6s",hs[hi].n);
        size_t szs[]={64,256}; for(int si=0;si<2;si++){size_t sz=szs[si];uint8_t*b=malloc(sz);for(size_t i=0;i<sz;i++)b[i]=i*0x9D+0x37;
            int it=sz<1024?20000000/sz:200000; volatile uint64_t s=0; clock_t st=clock(); for(int i=0;i<it;i++)s^=hs[hi].f(b,sz,i); clock_t en=clock();
            printf(" %8.1f",sz*(double)it/((double)(en-st)/CLOCKS_PER_SEC)/1e9); free(b);}
        uint8_t buf[256];for(int i=0;i<256;i++)buf[i]=i*0x9D+0x37;uint64_t base=hs[hi].f(buf,256,0);double lo=100;
        for(int by=0;by<32;by++)for(int bi=0;bi<8;bi++){buf[by]^=(1u<<bi);uint64_t hv=hs[hi].f(buf,256,0);buf[by]^=(1u<<bi);double p=pop(base^hv)/64.0*100;if(p<lo)lo=p;}
        int bins[256]={0};for(int i=0;i<256000;i++)bins[hs[hi].f(&i,4,i)&255]++;double ex=1000.0,chi2=0;for(int b=0;b<256;b++){double d=bins[b]-ex;chi2+=d*d/ex;}
        printf(" %5.1f%% %7.1f\n",lo,chi2);}
    return 0;
}
