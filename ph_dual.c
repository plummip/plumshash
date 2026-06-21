#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
static inline uint64_t rot(uint64_t x,int k){return(x<<k)|(x>>(64-k));}
#define PHI 0x9E3779B97F4A7C15ULL
#define M1 0x85EBCA77C2B2AE3DULL
#define M2 0xBF58476D1CE4E5B9ULL
#define M3 0x94D049BB133111EBULL

/* v4l: baseline 4-lane, 32B/iter */
uint64_t v4l(const void*b,size_t l,uint64_t s){const uint8_t*p=b,*e=p+l;uint64_t ba=s^(l*PHI),h1=ba*PHI,h2=ba*M1,h3=ba*M2,h4=ba*M3;
while(p+32<=e){h1^=rot(*(const uint64_t*)p,11);h1*=PHI;p+=8;h2^=rot(*(const uint64_t*)p,19);h2*=PHI;p+=8;h3^=rot(*(const uint64_t*)p,35);h3*=PHI;p+=8;h4^=rot(*(const uint64_t*)p,47);h4*=PHI;p+=8;}
while(p+8<=e){h1^=rot(*(const uint64_t*)p,11);h1*=PHI;p+=8;}
uint64_t t=0;switch(e-p){case 7:t^=(uint64_t)p[6]<<48;case 6:t^=(uint64_t)p[5]<<40;case 5:t^=(uint64_t)p[4]<<32;case 4:t^=(uint64_t)p[3]<<24;case 3:t^=(uint64_t)p[2]<<16;case 2:t^=(uint64_t)p[1]<<8;case 1:t^=p[0];h1^=rot(t,11);h1*=PHI;}
h1^=h2;h3^=h4;h1^=h3;h1^=h1>>29;h1*=M1;h1^=h1>>31;h1*=M2;h1^=h1>>37;h1*=PHI;h1^=h1>>41;return h1;}

/* v4d: 4-lane dual-word: 64B/iter, 2 words per lane */
uint64_t v4d(const void*b,size_t l,uint64_t s){const uint8_t*p=b,*e=p+l;uint64_t ba=s^(l*PHI),h1=ba*PHI,h2=ba*M1,h3=ba*M2,h4=ba*M3;
while(p+64<=e){
    h1^=rot(*(const uint64_t*)p,11);h1*=PHI;p+=8;h1^=rot(*(const uint64_t*)p,19);h1*=PHI;p+=8;
    h2^=rot(*(const uint64_t*)p,35);h2*=PHI;p+=8;h2^=rot(*(const uint64_t*)p,47);h2*=PHI;p+=8;
    h3^=rot(*(const uint64_t*)p,11);h3*=PHI;p+=8;h3^=rot(*(const uint64_t*)p,19);h3*=PHI;p+=8;
    h4^=rot(*(const uint64_t*)p,35);h4*=PHI;p+=8;h4^=rot(*(const uint64_t*)p,47);h4*=PHI;p+=8;
}
while(p+8<=e){h1^=rot(*(const uint64_t*)p,11);h1*=PHI;p+=8;}
uint64_t t=0;switch(e-p){case 7:t^=(uint64_t)p[6]<<48;case 6:t^=(uint64_t)p[5]<<40;case 5:t^=(uint64_t)p[4]<<32;case 4:t^=(uint64_t)p[3]<<24;case 3:t^=(uint64_t)p[2]<<16;case 2:t^=(uint64_t)p[1]<<8;case 1:t^=p[0];h1^=rot(t,11);h1*=PHI;}
h1^=h2;h3^=h4;h1^=h3;h1^=h1>>29;h1*=M1;h1^=h1>>31;h1*=M2;h1^=h1>>37;h1*=PHI;h1^=h1>>41;return h1;}

/* v4d2: same but each lane uses its OWN rotation pair (r1,r2) for lane1 etc */
uint64_t v4d2(const void*b,size_t l,uint64_t s){const uint8_t*p=b,*e=p+l;uint64_t ba=s^(l*PHI),h1=ba*PHI,h2=ba*M1,h3=ba*M2,h4=ba*M3;
while(p+64<=e){
    h1^=rot(*(const uint64_t*)p,11);h1*=PHI;p+=8;h1^=rot(*(const uint64_t*)p,43);h1*=PHI;p+=8;
    h2^=rot(*(const uint64_t*)p,19);h2*=PHI;p+=8;h2^=rot(*(const uint64_t*)p,53);h2*=PHI;p+=8;
    h3^=rot(*(const uint64_t*)p,35);h3*=PHI;p+=8;h3^=rot(*(const uint64_t*)p,13);h3*=PHI;p+=8;
    h4^=rot(*(const uint64_t*)p,47);h4*=PHI;p+=8;h4^=rot(*(const uint64_t*)p,59);h4*=PHI;p+=8;
}
while(p+8<=e){h1^=rot(*(const uint64_t*)p,11);h1*=PHI;p+=8;}
uint64_t t=0;switch(e-p){case 7:t^=(uint64_t)p[6]<<48;case 6:t^=(uint64_t)p[5]<<40;case 5:t^=(uint64_t)p[4]<<32;case 4:t^=(uint64_t)p[3]<<24;case 3:t^=(uint64_t)p[2]<<16;case 2:t^=(uint64_t)p[1]<<8;case 1:t^=p[0];h1^=rot(t,11);h1*=PHI;}
h1^=h2;h3^=h4;h1^=h3;h1^=h1>>29;h1*=M1;h1^=h1>>31;h1*=M2;h1^=h1>>37;h1*=PHI;h1^=h1>>41;return h1;}

static int pop(uint64_t x){x-=x>>1&0x5555555555555555ULL;x=(x&0x3333333333333333ULL)+(x>>2&0x3333333333333333ULL);x=(x+(x>>4))&0x0F0F0F0F0F0F0F0FULL;return(x*0x0101010101010101ULL)>>56;}

int main(void){
    printf("=== SPEED ===\n%8s","");
    size_t szs[]={64,256,1024,8192,65536,262144,1048576};int ns=7;
    struct{const char*n;void*f;}hs[]={{"v4l",v4l},{"v4d",v4d},{"v4d2",v4d2}};int nh=3;
    uint8_t wb[256];volatile uint64_t ws=0;for(int i=0;i<1000;i++){ws^=v4l(wb,256,i);ws^=v4d(wb,256,i);ws^=v4d2(wb,256,i);}
    for(int si=0;si<ns;si++)printf(" %8zu",szs[si]);printf("\n");
    for(int hi=0;hi<nh;hi++){printf("%-8s",hs[hi].n);typedef uint64_t(*hf)(const void*,size_t,uint64_t);hf fn=(hf)hs[hi].f;
        for(int si=0;si<ns;si++){size_t sz=szs[si];uint8_t*b=malloc(sz);for(size_t i=0;i<sz;i++)b[i]=i*0x9D+0x37;
            int it=sz<1024?10000000/sz:500000;if(it<100)it=100;volatile uint64_t s=0;clock_t st=clock();
            for(int i=0;i<it;i++)s^=fn(b,sz,i);clock_t en=clock();
            printf(" %8.1f",sz*(double)it/((double)(en-st)/CLOCKS_PER_SEC)/1e9);free(b);}printf(" GB/s\n");}
    printf("\n=== AVALANCHE ===\n");
    for(int hi=0;hi<nh;hi++){typedef uint64_t(*hf)(const void*,size_t,uint64_t);hf fn=(hf)hs[hi].f;
        uint8_t buf[256];for(int i=0;i<256;i++)buf[i]=i*0x9D+0x37;uint64_t base=fn(buf,256,0);double lo=100,s=0;
        for(int by=0;by<32;by++)for(int bi=0;bi<8;bi++){buf[by]^=(1u<<bi);uint64_t hv=fn(buf,256,0);buf[by]^=(1u<<bi);
            double p=pop(base^hv)/64.0*100;s+=p;if(p<lo)lo=p;}printf("%-8s worst=%.1f%%\n",hs[hi].n,lo);}
    return 0;
}
