#include <stdlib.h>
#define PLUMHASH_IMPLEMENTATION
/* Patch: lower threshold to 128 */
#undef PLUMHASH_IMPLEMENTATION
#include <stdint.h>
#include <stddef.h>
#include <string.h>
static inline uint64_t pl_rot(uint64_t x,int k){return(x<<k)|(x>>(64-k));}
#define PL_PHI 0x9E3779B97F4A7C15ULL
#define PL_M1  0x85EBCA77C2B2AE3DULL
#define PL_M2  0xBF58476D1CE4E5B9ULL
#define PL_M3  0x94D049BB133111EBULL

static uint64_t fast(const uint8_t*p,size_t l,uint64_t s){const uint8_t*e=p+l;uint64_t ba=s^(l*PL_PHI),h1=ba*PL_PHI,h2=ba*PL_M1,h3=ba*PL_M2,h4=ba*PL_M3;while(p+32<=e){h1^=*(const uint64_t*)p;p+=8;h1=pl_rot(h1+h2,11);h2^=*(const uint64_t*)p;p+=8;h2=pl_rot(h2+h3,17);h3^=*(const uint64_t*)p;p+=8;h3=pl_rot(h3+h4,23);h4^=*(const uint64_t*)p;p+=8;h4=pl_rot(h4+h1,57);}while(p+8<=e){h1^=*(const uint64_t*)p;p+=8;h1=pl_rot(h1+h2,11);}uint64_t t=0;switch(e-p){case 7:t^=(uint64_t)p[6]<<48;case 6:t^=(uint64_t)p[5]<<40;case 5:t^=(uint64_t)p[4]<<32;case 4:t^=(uint64_t)p[3]<<24;case 3:t^=(uint64_t)p[2]<<16;case 2:t^=(uint64_t)p[1]<<8;case 1:t^=p[0];h1^=t;h1=pl_rot(h1+h2,11);}h1^=h2;h3^=h4;h1^=h3;h1^=h1>>29;h1*=PL_M1;h1^=h1>>31;h1*=PL_M2;h1^=h1>>37;h1*=PL_PHI;h1^=h1>>41;return h1;}

static uint64_t safe(const uint8_t*p,size_t l,uint64_t s){const uint8_t*e=p+l;uint64_t ba=s^(l*PL_PHI),h1=ba*PL_PHI,h2=ba*PL_M1,h3=ba*PL_M2,h4=ba*PL_M3,acc=ba^PL_M2;int hb=0;while(p+32<=e){uint64_t v1=*(const uint64_t*)p;p+=8;uint64_t v2=*(const uint64_t*)p;p+=8;uint64_t v3=*(const uint64_t*)p;p+=8;uint64_t v4=*(const uint64_t*)p;p+=8;h1^=v1;h1=pl_rot(h1+h2,11);h2^=v2;h2=pl_rot(h2+h3,17);h3^=v3;h3=pl_rot(h3+h4,23);h4^=v4;h4=pl_rot(h4+h1,57);acc^=v1^v2^v3^v4;acc=pl_rot(acc,31);acc*=PL_PHI;hb=1;}while(p+8<=e){uint64_t v=*(const uint64_t*)p;p+=8;h1^=v;h1=pl_rot(h1+h2,11);acc^=v;acc=pl_rot(acc,31);acc*=PL_PHI;hb=1;}h2^=h1;h2=pl_rot(h2,43);h1^=h2;uint64_t t=0;switch(e-p){case 7:t^=(uint64_t)p[6]<<48;case 6:t^=(uint64_t)p[5]<<40;case 5:t^=(uint64_t)p[4]<<32;case 4:t^=(uint64_t)p[3]<<24;case 3:t^=(uint64_t)p[2]<<16;case 2:t^=(uint64_t)p[1]<<8;case 1:t^=p[0];}if(t){h1^=t;h1=pl_rot(h1+h2,11);if(hb){acc^=t;acc=pl_rot(acc,31);acc*=PL_PHI;}}h1^=h2;h3^=h4;h1^=h3;if(hb)h1^=acc;h1^=h1>>29;h1*=PL_M1;h1^=h1>>31;h1*=PL_M2;h1^=h1>>37;h1*=PL_PHI;h1^=h1>>41;return h1;}

uint64_t ph128(const void*b,size_t l,uint64_t s){return l>=128?fast((const uint8_t*)b,l,s):safe((const uint8_t*)b,l,s);}

#include <stdio.h>
#include <time.h>
#include <math.h>
static inline int pop(uint64_t x){x-=x>>1&0x5555555555555555ULL;x=(x&0x3333333333333333ULL)+(x>>2&0x3333333333333333ULL);x=(x+(x>>4))&0x0F0F0F0F0F0F0F0FULL;return(x*0x0101010101010101ULL)>>56;}

int main(void){
    printf("Threshold 128 test:\n");
    /* ava */
    uint8_t buf[256];for(int i=0;i<256;i++)buf[i]=i*0x9D+0x37;uint64_t base=ph128(buf,256,0);double lo=100;
    for(int by=0;by<32;by++)for(int bi=0;bi<8;bi++){buf[by]^=(1u<<bi);uint64_t hv=ph128(buf,256,0);buf[by]^=(1u<<bi);double p=pop(base^hv)/64.0*100;if(p<lo)lo=p;}
    /* chi2 */
    int bins[256]={0};for(int i=0;i<256000;i++)bins[ph128(&i,4,i)&255]++;double ex=1000.0,chi2=0;for(int b=0;b<256;b++){double d=bins[b]-ex;chi2+=d*d/ex;}
    /* sparse */
    int cols=0;uint64_t seen[20000]={0};int ngen=0;uint8_t key[256];for(int pos=0;pos<128&&ngen<20000;pos++)for(int val=1;val<256&&ngen<20000;val++){int kl=8+(pos%57);memset(key,0,kl);key[kl-1]=val;if(pos&1)key[0]=pos^val;seen[ngen++]=ph128(key,kl,0);}for(int i=0;i<ngen;i++)for(int j=i+1;j<ngen;j++)if(seen[i]==seen[j])cols++;
    /* speed */
    double spd[4];size_t szs[]={64,128,256,1024};
    for(int si=0;si<4;si++){size_t sz=szs[si];uint8_t*b=malloc(sz);for(size_t i=0;i<sz;i++)b[i]=i*0x9D+0x37;
        volatile uint64_t s=0;clock_t st=clock();for(int i=0;i<20000000/sz;i++)s^=ph128(b,sz,i);clock_t en=clock();
        spd[si]=sz*(20000000/sz)/((double)(en-st)/CLOCKS_PER_SEC)/1e9;free(b);}
    printf("ava=%.1f%% chi2=%.1f sparse=%d speed: 64=%.1f 128=%.1f 256=%.1f 1K=%.1f GB/s\n",lo,chi2,cols,spd[0],spd[1],spd[2],spd[3]);
    return 0;
}
