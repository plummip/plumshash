#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>
#include <time.h>
static inline uint64_t rd(const void*p){uint64_t v;memcpy(&v,p,8);return v;}
static inline uint64_t rot(uint64_t x,int k){return(x<<k)|(x>>(64-k));}
static inline int pop(uint64_t x){x-=x>>1&0x5555555555555555ULL;x=(x&0x3333333333333333ULL)+(x>>2&0x3333333333333333ULL);x=(x+(x>>4))&0x0F0F0F0F0F0F0F0FULL;return(x*0x0101010101010101ULL)>>56;}
#define PHI 0x9E3779B97F4A7C15ULL
#define M1 0x85EBCA77C2B2AE3DULL
#define M2 0xBF58476D1CE4E5B9ULL
#define M3 0x94D049BB133111EBULL
uint64_t ph(const void*b,size_t l,uint64_t s,int r1,int r2,int r3,int r4){
    const uint8_t*p=(const uint8_t*)b,*e=p+l;uint64_t ba=s^(l*PHI),h1=ba*PHI,h2=ba*M1,h3=ba*M2,h4=ba*M3;
    while(p+32<=e){h1^=rot(rd(p),r1);h1*=PHI;p+=8;h2^=rot(rd(p),r2);h2*=PHI;p+=8;h3^=rot(rd(p),r3);h3*=PHI;p+=8;h4^=rot(rd(p),r4);h4*=PHI;p+=8;}
    while(p+8<=e){h1^=rot(rd(p),r1);h1*=PHI;p+=8;}
    uint64_t t=0;switch(e-p){case 7:t^=(uint64_t)p[6]<<48;case 6:t^=(uint64_t)p[5]<<40;case 5:t^=(uint64_t)p[4]<<32;case 4:t^=(uint64_t)p[3]<<24;case 3:t^=(uint64_t)p[2]<<16;case 2:t^=(uint64_t)p[1]<<8;case 1:t^=(uint64_t)p[0];h1^=rot(t,r1);h1*=PHI;}
    h1^=h2;h3^=h4;h1^=h3;h1^=h1>>29;h1*=M1;h1^=h1>>31;h1*=M2;h1^=h1>>37;h1*=PHI;h1^=h1>>41;return h1;
}
int main(void){
    uint8_t buf[256];for(int i=0;i<256;i++)buf[i]=(uint8_t)(i*0x9D+0x37);
    /* Test each candidate with the ORIGINAL 4r finalizer */
    struct {int r1,r2,r3,r4;double w;} cands[]={
        {23,47,13,37,0}, /* v1 */
        {11,19,35,47,0}, /* scan best */
        {13,23,37,47,0}, /* symmetric variant */
        {17,29,41,53,0}, /* well-spaced primes */
        {11,23,37,53,0}, /* mixed */
        {13,31,43,59,0}, /* wide spread */
    };
    int nc=sizeof(cands)/sizeof(cands[0]);
    printf("Rotation set         | avalanche avg%% worst%% best%%\n");
    printf("─────────────────────┼─────────────────────────────\n");
    for(int ci=0;ci<nc;ci++){
        int r1=cands[ci].r1,r2=cands[ci].r2,r3=cands[ci].r3,r4=cands[ci].r4;
        uint64_t base=ph(buf,256,0,r1,r2,r3,r4);
        double s=0,lo=100,hi=0;
        for(int by=0;by<32;by++)for(int bi=0;bi<8;bi++){
            buf[by]^=(1u<<bi);uint64_t hv=ph(buf,256,0,r1,r2,r3,r4);buf[by]^=(1u<<bi);
            double p=pop(base^hv)/64.0*100;s+=p;if(p<lo)lo=p;if(p>hi)hi=p;
        }
        printf("{%2d,%2d,%2d,%2d}           | %.1f     %.1f     %.1f\n",r1,r2,r3,r4,s/(32*8),lo,hi);
    }
    return 0;
}
