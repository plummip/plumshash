#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>

static inline uint64_t rotl64(uint64_t x,int k){return(x<<k)|(x>>(64-k));}
#define PHI 0x9E3779B97F4A7C15ULL
#define M1 0x85EBCA77C2B2AE3DULL
#define M2 0xBF58476D1CE4E5B9ULL
#define M3 0x94D049BB133111EBULL

/* 8 rotation constants: well-spaced odd numbers 11-47, all tested as good individually */
#define R1 11
#define R2 15
#define R3 19
#define R4 23
#define R5 27
#define R6 31
#define R7 35
#define R8 47

/* v8l: 8-lane, 64 bytes per iteration */
uint64_t v8l(const void *b, size_t l, uint64_t s){
    const uint8_t *p=(const uint8_t*)b,*e=p+l;
    uint64_t ba=s^(l*PHI);
    uint64_t h1=ba*PHI,h2=ba*M1,h3=ba*M2,h4=ba*M3;
    uint64_t h5=ba*0xA3C8B9D5E1F2A7B4ULL,h6=ba*0xC6A4A7935BD1E995ULL;
    uint64_t h7=ba*0x9C3B7A5D1E8F6B2AULL,h8=ba*0xD4E5F6A7B8C9D0E1ULL;
    while(p+64<=e){
        h1^=rotl64(*(const uint64_t*)p,R1);h1*=PHI;p+=8;
        h2^=rotl64(*(const uint64_t*)p,R2);h2*=PHI;p+=8;
        h3^=rotl64(*(const uint64_t*)p,R3);h3*=PHI;p+=8;
        h4^=rotl64(*(const uint64_t*)p,R4);h4*=PHI;p+=8;
        h5^=rotl64(*(const uint64_t*)p,R5);h5*=PHI;p+=8;
        h6^=rotl64(*(const uint64_t*)p,R6);h6*=PHI;p+=8;
        h7^=rotl64(*(const uint64_t*)p,R7);h7*=PHI;p+=8;
        h8^=rotl64(*(const uint64_t*)p,R8);h8*=PHI;p+=8;
    }
    while(p+8<=e){h1^=rotl64(*(const uint64_t*)p,R1);h1*=PHI;p+=8;}
    uint64_t t=0;switch(e-p){
        case 7:t^=(uint64_t)p[6]<<48;case 6:t^=(uint64_t)p[5]<<40;case 5:t^=(uint64_t)p[4]<<32;
        case 4:t^=(uint64_t)p[3]<<24;case 3:t^=(uint64_t)p[2]<<16;case 2:t^=(uint64_t)p[1]<<8;
        case 1:t^=p[0];h1^=rotl64(t,R1);h1*=PHI;
    }
    /* 8→4→2→1 tree reduction */
    h1^=h2;h3^=h4;h5^=h6;h7^=h8;
    h1*=PHI;h3*=PHI;h5*=PHI;h7*=PHI;
    h1^=h3;h5^=h7;
    h1*=PHI;h5*=PHI;
    h1^=h5;
    h1*=PHI;
    /* 4-round finalizer */
    h1^=h1>>29;h1*=M1;h1^=h1>>31;h1*=M2;h1^=h1>>37;h1*=PHI;h1^=h1>>41;
    return h1;
}

/* v4l: original 4-lane for comparison */
uint64_t v4l(const void *b, size_t l, uint64_t s){
    const uint8_t *p=(const uint8_t*)b,*e=p+l;
    uint64_t ba=s^(l*PHI),h1=ba*PHI,h2=ba*M1,h3=ba*M2,h4=ba*M3;
    while(p+32<=e){
        h1^=rotl64(*(const uint64_t*)p,11);h1*=PHI;p+=8;
        h2^=rotl64(*(const uint64_t*)p,19);h2*=PHI;p+=8;
        h3^=rotl64(*(const uint64_t*)p,35);h3*=PHI;p+=8;
        h4^=rotl64(*(const uint64_t*)p,47);h4*=PHI;p+=8;
    }
    while(p+8<=e){h1^=rotl64(*(const uint64_t*)p,11);h1*=PHI;p+=8;}
    uint64_t t=0;switch(e-p){
        case 7:t^=(uint64_t)p[6]<<48;case 6:t^=(uint64_t)p[5]<<40;case 5:t^=(uint64_t)p[4]<<32;
        case 4:t^=(uint64_t)p[3]<<24;case 3:t^=(uint64_t)p[2]<<16;case 2:t^=(uint64_t)p[1]<<8;
        case 1:t^=p[0];h1^=rotl64(t,11);h1*=PHI;
    }
    h1^=h2;h3^=h4;h1^=h3;h1^=h1>>29;h1*=M1;h1^=h1>>31;h1*=M2;h1^=h1>>37;h1*=PHI;h1^=h1>>41;return h1;
}

static int pop(uint64_t x){x-=x>>1&0x5555555555555555ULL;x=(x&0x3333333333333333ULL)+(x>>2&0x3333333333333333ULL);x=(x+(x>>4))&0x0F0F0F0F0F0F0F0FULL;return(x*0x0101010101010101ULL)>>56;}

int main(void){
    /* Speed test */
    printf("=== SPEED (GB/s) ===\n%8s","");
    size_t szs[]={64,256,1024,8192,65536,262144,1048576};
    int ns=7;
    struct{const char*n;void*f;}hs[]={{"4-lane",v4l},{"8-lane",v8l}};
    int nh=2;

    uint8_t wb[256];volatile uint64_t ws=0;
    for(int i=0;i<1000;i++){ws^=v4l(wb,256,i);ws^=v8l(wb,256,i);}

    for(int si=0;si<ns;si++)printf(" %8zu",szs[si]);
    printf("\n");
    for(int hi=0;hi<nh;hi++){
        printf("%-8s",hs[hi].n);
        typedef uint64_t(*hf)(const void*,size_t,uint64_t);
        hf fn=(hf)hs[hi].f;
        for(int si=0;si<ns;si++){
            size_t sz=szs[si];
            uint8_t*buf=malloc(sz);for(size_t i=0;i<sz;i++)buf[i]=i*0x9D+0x37;
            int iters=sz<1024?10000000/sz:sz<65536?500000:50000;if(iters<100)iters=100;
            volatile uint64_t sum=0;clock_t st=clock();
            for(int i=0;i<iters;i++)sum^=fn(buf,sz,i);
            clock_t en=clock();
            double gb=sz*(double)iters/((double)(en-st)/CLOCKS_PER_SEC)/1e9;
            printf(" %8.1f",gb);free(buf);
        }
        printf(" GB/s\n");
    }

    /* Quality: avalanche */
    printf("\n=== AVALANCHE ===\n");
    for(int hi=0;hi<nh;hi++){
        typedef uint64_t(*hf)(const void*,size_t,uint64_t);
        hf fn=(hf)hs[hi].f;
        uint8_t buf[256];for(int i=0;i<256;i++)buf[i]=i*0x9D+0x37;
        uint64_t base=fn(buf,256,0);double lo=100;double s=0;
        for(int by=0;by<32;by++)for(int bi=0;bi<8;bi++){
            buf[by]^=(1u<<bi);uint64_t hv=fn(buf,256,0);buf[by]^=(1u<<bi);
            double p=pop(base^hv)/64.0*100;s+=p;if(p<lo)lo=p;
        }
        printf("%-8s avg=%.1f%% worst=%.1f%%\n",hs[hi].n,s/(32*8),lo);
    }

    /* Quality: distribution */
    printf("\n=== CHI-SQUARED ===\n");
    for(int hi=0;hi<nh;hi++){
        typedef uint64_t(*hf)(const void*,size_t,uint64_t);
        hf fn=(hf)hs[hi].f;
        int bins[256]={0};
        for(int i=0;i<256000;i++){
            uint64_t hv=fn(&i,4,i);
            bins[hv&255]++;
        }
        double expv=1000.0,chi2=0;
        for(int b=0;b<256;b++){double d=bins[b]-expv;chi2+=d*d/expv;}
        printf("%-8s chi2=%.1f (df=255)\n",hs[hi].n,chi2);
    }
    return 0;
}
