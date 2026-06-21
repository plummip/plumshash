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

/* v2: baseline (memcpy, 1x loop) */
uint64_t v2(const void *b, size_t l, uint64_t s) {
    const uint8_t *p=(const uint8_t*)b,*e=p+l;
    uint64_t ba=s^(l*PHI),h1=ba*PHI,h2=ba*M1,h3=ba*M2,h4=ba*M3;
    while(p+32<=e){
        uint64_t v;memcpy(&v,p,8);h1^=rotl64(v,11);h1*=PHI;p+=8;
        memcpy(&v,p,8);h2^=rotl64(v,19);h2*=PHI;p+=8;
        memcpy(&v,p,8);h3^=rotl64(v,35);h3*=PHI;p+=8;
        memcpy(&v,p,8);h4^=rotl64(v,47);h4*=PHI;p+=8;
    }
    while(p+8<=e){uint64_t v;memcpy(&v,p,8);h1^=rotl64(v,11);h1*=PHI;p+=8;}
    uint64_t t=0;switch(e-p){
        case 7:t^=(uint64_t)p[6]<<48;case 6:t^=(uint64_t)p[5]<<40;
        case 5:t^=(uint64_t)p[4]<<32;case 4:t^=(uint64_t)p[3]<<24;
        case 3:t^=(uint64_t)p[2]<<16;case 2:t^=(uint64_t)p[1]<<8;
        case 1:t^=p[0];h1^=rotl64(t,11);h1*=PHI;
    }
    h1^=h2;h3^=h4;h1^=h3;h1^=h1>>29;h1*=M1;h1^=h1>>31;h1*=M2;h1^=h1>>37;h1*=PHI;h1^=h1>>41;return h1;
}

/* v2d: direct loads (no memcpy), otherwise identical to v2 */
uint64_t v2d(const void *b, size_t l, uint64_t s) {
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
        case 7:t^=(uint64_t)p[6]<<48;case 6:t^=(uint64_t)p[5]<<40;
        case 5:t^=(uint64_t)p[4]<<32;case 4:t^=(uint64_t)p[3]<<24;
        case 3:t^=(uint64_t)p[2]<<16;case 2:t^=(uint64_t)p[1]<<8;
        case 1:t^=p[0];h1^=rotl64(t,11);h1*=PHI;
    }
    h1^=h2;h3^=h4;h1^=h3;h1^=h1>>29;h1*=M1;h1^=h1>>31;h1*=M2;h1^=h1>>37;h1*=PHI;h1^=h1>>41;return h1;
}

/* v4: 4x unrolled, direct loads, prefetch */
uint64_t v4(const void *b, size_t l, uint64_t s) {
    const uint8_t *p=(const uint8_t*)b,*e=p+l;
    uint64_t ba=s^(l*PHI),h1=ba*PHI,h2=ba*M1,h3=ba*M2,h4=ba*M3;
    while(p+128<=e){
        #define B(h,r) h^=rotl64(*(const uint64_t*)p,r);h*=PHI;p+=8;
        B(h1,11);B(h2,19);B(h3,35);B(h4,47);B(h1,11);B(h2,19);B(h3,35);B(h4,47);
        B(h1,11);B(h2,19);B(h3,35);B(h4,47);B(h1,11);B(h2,19);B(h3,35);B(h4,47);
        #undef B
    }
    while(p+32<=e){
        h1^=rotl64(*(const uint64_t*)p,11);h1*=PHI;p+=8;
        h2^=rotl64(*(const uint64_t*)p,19);h2*=PHI;p+=8;
        h3^=rotl64(*(const uint64_t*)p,35);h3*=PHI;p+=8;
        h4^=rotl64(*(const uint64_t*)p,47);h4*=PHI;p+=8;
    }
    while(p+8<=e){h1^=rotl64(*(const uint64_t*)p,11);h1*=PHI;p+=8;}
    uint64_t t=0;switch(e-p){
        case 7:t^=(uint64_t)p[6]<<48;case 6:t^=(uint64_t)p[5]<<40;
        case 5:t^=(uint64_t)p[4]<<32;case 4:t^=(uint64_t)p[3]<<24;
        case 3:t^=(uint64_t)p[2]<<16;case 2:t^=(uint64_t)p[1]<<8;
        case 1:t^=p[0];h1^=rotl64(t,11);h1*=PHI;
    }
    h1^=h2;h3^=h4;h1^=h3;h1^=h1>>29;h1*=M1;h1^=h1>>31;h1*=M2;h1^=h1>>37;h1*=PHI;h1^=h1>>41;return h1;
}

/* v8: 8x unrolled */
uint64_t v8(const void *b, size_t l, uint64_t s) {
    const uint8_t *p=(const uint8_t*)b,*e=p+l;
    uint64_t ba=s^(l*PHI),h1=ba*PHI,h2=ba*M1,h3=ba*M2,h4=ba*M3;
    while(p+256<=e){
        #define B(h,r) h^=rotl64(*(const uint64_t*)p,r);h*=PHI;p+=8;
        B(h1,11);B(h2,19);B(h3,35);B(h4,47);B(h1,11);B(h2,19);B(h3,35);B(h4,47);
        B(h1,11);B(h2,19);B(h3,35);B(h4,47);B(h1,11);B(h2,19);B(h3,35);B(h4,47);
        B(h1,11);B(h2,19);B(h3,35);B(h4,47);B(h1,11);B(h2,19);B(h3,35);B(h4,47);
        B(h1,11);B(h2,19);B(h3,35);B(h4,47);B(h1,11);B(h2,19);B(h3,35);B(h4,47);
        #undef B
    }
    while(p+32<=e){
        h1^=rotl64(*(const uint64_t*)p,11);h1*=PHI;p+=8;
        h2^=rotl64(*(const uint64_t*)p,19);h2*=PHI;p+=8;
        h3^=rotl64(*(const uint64_t*)p,35);h3*=PHI;p+=8;
        h4^=rotl64(*(const uint64_t*)p,47);h4*=PHI;p+=8;
    }
    while(p+8<=e){h1^=rotl64(*(const uint64_t*)p,11);h1*=PHI;p+=8;}
    uint64_t t=0;switch(e-p){
        case 7:t^=(uint64_t)p[6]<<48;case 6:t^=(uint64_t)p[5]<<40;
        case 5:t^=(uint64_t)p[4]<<32;case 4:t^=(uint64_t)p[3]<<24;
        case 3:t^=(uint64_t)p[2]<<16;case 2:t^=(uint64_t)p[1]<<8;
        case 1:t^=p[0];h1^=rotl64(t,11);h1*=PHI;
    }
    h1^=h2;h3^=h4;h1^=h3;h1^=h1>>29;h1*=M1;h1^=h1>>31;h1*=M2;h1^=h1>>37;h1*=PHI;h1^=h1>>41;return h1;
}

int main(void){
    enum{KB=1024,MB=1024*KB};
    size_t sizes[]={8,32,64,256,KB,8*KB,64*KB,256*KB,MB};
    int ns=sizeof(sizes)/sizeof(sizes[0]);

    typedef uint64_t(*hf)(const void*,size_t,uint64_t);
    struct{const char*n;hf f;} hashes[]={{"v2",v2},{"v2d",v2d},{"v4",v4},{"v8",v8}};
    int nh=sizeof(hashes)/sizeof(hashes[0]);

    for(int hi=0;hi<nh;hi++){
        printf("%-4s ",hashes[hi].n);
        for(int si=0;si<ns;si++){
            size_t sz=sizes[si];
            uint8_t*buf=malloc(sz);for(size_t i=0;i<sz;i++)buf[i]=i*0x9D+0x37;
            int iters=sz<256?50000000/sz:10000000/(sz/256+1);if(iters<100)iters=100;
            volatile uint64_t sum=0;
            clock_t st=clock();
            for(int i=0;i<iters;i++)sum^=hashes[hi].f(buf,sz,i);
            clock_t en=clock();
            double secs=(double)(en-st)/CLOCKS_PER_SEC;
            double gb=sz*(double)iters/secs/1e9;
            printf("%s%.1f",si?",":"",gb);
            free(buf);if(secs<0.1)break; /* skip tiny sizes if too fast */
        }
        printf(" GB/s\n");
    }
    return 0;
}
