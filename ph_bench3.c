#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>

static inline uint64_t rotl64(uint64_t x, int k) { return (x<<k)|(x>>(64-k)); }
#define PHI 0x9E3779B97F4A7C15ULL
#define M1 0x85EBCA77C2B2AE3DULL
#define M2 0xBF58476D1CE4E5B9ULL
#define M3 0x94D049BB133111EBULL

/* Multiply-mix: returns lo ^ hi from 64x64→128 multiply */
static inline uint64_t mulmix(uint64_t a, uint64_t b) {
    __uint128_t p = (__uint128_t)a * b;
    return (uint64_t)p ^ (uint64_t)(p >> 64);
}

/* v2d: baseline with direct loads */
uint64_t v2d(const void *b, size_t l, uint64_t s) {
    const uint8_t *p=(const uint8_t*)b,*e=p+l;
    uint64_t ba=s^(l*PHI),h1=ba*PHI,h2=ba*M1,h3=ba*M2,h4=ba*M3;
    while(p+32<=e){h1^=rotl64(*(const uint64_t*)p,11);h1*=PHI;p+=8;h2^=rotl64(*(const uint64_t*)p,19);h2*=PHI;p+=8;h3^=rotl64(*(const uint64_t*)p,35);h3*=PHI;p+=8;h4^=rotl64(*(const uint64_t*)p,47);h4*=PHI;p+=8;}
    while(p+8<=e){h1^=rotl64(*(const uint64_t*)p,11);h1*=PHI;p+=8;}
    uint64_t t=0;switch(e-p){case 7:t^=(uint64_t)p[6]<<48;case 6:t^=(uint64_t)p[5]<<40;case 5:t^=(uint64_t)p[4]<<32;case 4:t^=(uint64_t)p[3]<<24;case 3:t^=(uint64_t)p[2]<<16;case 2:t^=(uint64_t)p[1]<<8;case 1:t^=p[0];h1^=rotl64(t,11);h1*=PHI;}
    h1^=h2;h3^=h4;h1^=h3;h1^=h1>>29;h1*=M1;h1^=h1>>31;h1*=M2;h1^=h1>>37;h1*=PHI;h1^=h1>>41;return h1;
}

/* v5: mulmix in body loop (stronger mixing, may affect quality) */
uint64_t v5(const void *b, size_t l, uint64_t s) {
    const uint8_t *p=(const uint8_t*)b,*e=p+l;
    uint64_t ba=s^(l*PHI),h1=ba*PHI,h2=ba*M1,h3=ba*M2,h4=ba*M3;
    while(p+32<=e){
        h1^=rotl64(*(const uint64_t*)p,11);h1=mulmix(h1,PHI);p+=8;
        h2^=rotl64(*(const uint64_t*)p,19);h2=mulmix(h2,PHI);p+=8;
        h3^=rotl64(*(const uint64_t*)p,35);h3=mulmix(h3,PHI);p+=8;
        h4^=rotl64(*(const uint64_t*)p,47);h4=mulmix(h4,PHI);p+=8;
    }
    while(p+8<=e){h1^=rotl64(*(const uint64_t*)p,11);h1=mulmix(h1,PHI);p+=8;}
    uint64_t t=0;switch(e-p){case 7:t^=(uint64_t)p[6]<<48;case 6:t^=(uint64_t)p[5]<<40;case 5:t^=(uint64_t)p[4]<<32;case 4:t^=(uint64_t)p[3]<<24;case 3:t^=(uint64_t)p[2]<<16;case 2:t^=(uint64_t)p[1]<<8;case 1:t^=p[0];h1^=rotl64(t,11);h1=mulmix(h1,PHI);}
    h1^=h2;h3^=h4;h1^=h3;h1=mulmix(h1,PHI);
    h1^=h1>>29;h1=mulmix(h1,M1);h1^=h1>>31;h1=mulmix(h1,M2);h1^=h1>>37;h1=mulmix(h1,PHI);h1^=h1>>41;
    return h1;
}

/* v6: mulmix only in finalizer (safer, cheaper) */
uint64_t v6(const void *b, size_t l, uint64_t s) {
    const uint8_t *p=(const uint8_t*)b,*e=p+l;
    uint64_t ba=s^(l*PHI),h1=ba*PHI,h2=ba*M1,h3=ba*M2,h4=ba*M3;
    while(p+32<=e){h1^=rotl64(*(const uint64_t*)p,11);h1*=PHI;p+=8;h2^=rotl64(*(const uint64_t*)p,19);h2*=PHI;p+=8;h3^=rotl64(*(const uint64_t*)p,35);h3*=PHI;p+=8;h4^=rotl64(*(const uint64_t*)p,47);h4*=PHI;p+=8;}
    while(p+8<=e){h1^=rotl64(*(const uint64_t*)p,11);h1*=PHI;p+=8;}
    uint64_t t=0;switch(e-p){case 7:t^=(uint64_t)p[6]<<48;case 6:t^=(uint64_t)p[5]<<40;case 5:t^=(uint64_t)p[4]<<32;case 4:t^=(uint64_t)p[3]<<24;case 3:t^=(uint64_t)p[2]<<16;case 2:t^=(uint64_t)p[1]<<8;case 1:t^=p[0];h1^=rotl64(t,11);h1*=PHI;}
    h1^=h2;h3^=h4;h1^=h3;h1=mulmix(h1,PHI);
    h1^=h1>>29;h1=mulmix(h1,M1);h1^=h1>>31;h1=mulmix(h1,M2);h1^=h1>>37;h1=mulmix(h1,PHI);h1^=h1>>41;
    return h1;
}

static double bench(void*f,size_t sz,int iters){
    uint8_t*buf=malloc(sz);for(size_t i=0;i<sz;i++)buf[i]=i*0x9D+0x37;
    volatile uint64_t sum=0;
    typedef uint64_t(*hf)(const void*,size_t,uint64_t);
    hf h=(hf)f;
    clock_t st=clock();for(int i=0;i<iters;i++)sum^=h(buf,sz,i);clock_t en=clock();
    free(buf);
    return sz*(double)iters/((double)(en-st)/CLOCKS_PER_SEC)/1e9;
}

/* quick quality check for v5/v6 vs v2d */
static int pop(uint64_t x){x-=x>>1&0x5555555555555555ULL;x=(x&0x3333333333333333ULL)+(x>>2&0x3333333333333333ULL);x=(x+(x>>4))&0x0F0F0F0F0F0F0F0FULL;return(x*0x0101010101010101ULL)>>56;}

int main(void){
    /* part 1: speed */
    printf("=== SPEED (GB/s) ===\n");
    size_t sz[]={64,256,1024,8192,65536,262144};
    int ns=6;
    struct{const char*n;void*f;}h[]={{"v2d",v2d},{"v5",v5},{"v6",v6}};
    int nh=3;
    uint8_t wb[256];volatile uint64_t ws=0;
    for(int i=0;i<1000;i++){ws^=v2d(wb,256,i);ws^=v5(wb,256,i);ws^=v6(wb,256,i);}
    for(int hi=0;hi<nh;hi++){
        printf("%-4s",h[hi].n);
        for(int si=0;si<ns;si++){
            int iters=sz[si]<1024?5000000:sz[si]<65536?500000:50000;
            if(iters<100)iters=100;
            printf(" %5zu:%.1f",sz[si],bench(h[hi].f,sz[si],iters));
        }
        printf(" GB/s\n");
    }

    /* part 2: quality (avalanche) */
    printf("\n=== AVALANCHE (worst%%) ===\n");
    typedef uint64_t(*hf)(const void*,size_t,uint64_t);
    for(int hi=0;hi<nh;hi++){
        hf fn=(hf)h[hi].f;
        uint8_t buf[256];for(int i=0;i<256;i++)buf[i]=i*0x9D+0x37;
        uint64_t base=fn(buf,256,0);double lo=100;
        for(int by=0;by<32;by++)for(int bi=0;bi<8;bi++){
            buf[by]^=(1u<<bi);uint64_t hv=fn(buf,256,0);buf[by]^=(1u<<bi);
            double p=pop(base^hv)/64.0*100;if(p<lo)lo=p;
        }
        printf("%-4s worst=%.1f%%\n",h[hi].n,lo);
    }
    return 0;
}
