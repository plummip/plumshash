#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <math.h>
static inline uint64_t rot(uint64_t x,int k){return(x<<k)|(x>>(64-k));}
static inline int pop(uint64_t x){x-=x>>1&0x5555555555555555ULL;x=(x&0x3333333333333333ULL)+(x>>2&0x3333333333333333ULL);x=(x+(x>>4))&0x0F0F0F0F0F0F0F0FULL;return(x*0x0101010101010101ULL)>>56;}

/* Fast path with tunable init multipliers */
uint64_t hf(const void*b,size_t l,uint64_t s,uint64_t i1,uint64_t i2,uint64_t i3,uint64_t i4){
    const uint8_t*p=b,*e=p+l;uint64_t ba=s^(l*i1),h1=ba*i1,h2=ba*i2,h3=ba*i3,h4=ba*i4;
    while(p+32<=e){h1^=*(const uint64_t*)p;p+=8;h1=rot(h1+h2,11);h2^=*(const uint64_t*)p;p+=8;h2=rot(h2+h3,17);h3^=*(const uint64_t*)p;p+=8;h3=rot(h3+h4,23);h4^=*(const uint64_t*)p;p+=8;h4=rot(h4+h1,57);}
    while(p+8<=e){h1^=*(const uint64_t*)p;p+=8;h1=rot(h1+h2,11);}
    uint64_t t=0;switch(e-p){case 7:t^=(uint64_t)p[6]<<48;case 6:t^=(uint64_t)p[5]<<40;case 5:t^=(uint64_t)p[4]<<32;case 4:t^=(uint64_t)p[3]<<24;case 3:t^=(uint64_t)p[2]<<16;case 2:t^=(uint64_t)p[1]<<8;case 1:t^=p[0];h1^=t;h1=rot(h1+h2,11);}
    h1^=h2;h3^=h4;h1^=h3;h1^=h1>>29;h1*=i2;h1^=h1>>31;h1*=i3;h1^=h1>>37;h1*=i1;h1^=h1>>41;return h1;
}

int main(void){
    uint64_t cand[]={0x9E3779B97F4A7C15ULL,0x85EBCA77C2B2AE3DULL,0xBF58476D1CE4E5B9ULL,0x94D049BB133111EBULL,0xA3C8B9D5E1F2A7B4ULL,0xC6A4A7935BD1E995ULL};
    int nc=6; const char*cn[]={"PHI","M1","M2","M3","M4","M5"};
    uint64_t cur[]={0x9E3779B97F4A7C15ULL,0x85EBCA77C2B2AE3DULL,0xBF58476D1CE4E5B9ULL,0x94D049BB133111EBULL};
    
    printf("Init multiplier scan (fast path, 256B ava + 4B chi2):\n");
    printf("Current: {PHI,M1,M2,M3} ava=39.1%% chi2=238.4\n\n");
    printf("%6s %6s %6s %6s %6s %7s\n","i1","i2","i3","i4","ava%","chi2");
    
    double best_ava=0; int best_chi2=999; int bi1=0,bi2=0,bi3=0,bi4=0;
    /* Only test PHI/M1/M2/M3 as init (these are proven good). Test all 4! assignments. */
    for(int a=0;a<nc;a++)for(int b=0;b<nc;b++)for(int c=0;c<nc;c++)for(int d=0;d<nc;d++){
        if(a==b||a==c||a==d||b==c||b==d||c==d)continue;
        uint64_t i1=cand[a],i2=cand[b],i3=cand[c],i4=cand[d];
        /* ava quick check (first 8 bytes only for speed) */
        uint8_t buf[256];for(int i=0;i<256;i++)buf[i]=i*0x9D+0x37;uint64_t base=hf(buf,256,0,i1,i2,i3,i4);double lo=100;int fail=0;
        for(int by=0;by<32&&!fail;by++)for(int bi=0;bi<8&&!fail;bi++){buf[by]^=(1u<<bi);uint64_t hv=hf(buf,256,0,i1,i2,i3,i4);buf[by]^=(1u<<bi);double p=pop(base^hv)/64.0*100;if(p<lo)lo=p;if(p<25)fail=1;}
        if(fail)continue;
        /* chi2 */
        int bins[256]={0};for(int i=0;i<256000;i++)bins[hf(&i,4,i,i1,i2,i3,i4)&255]++;double ex=1000.0,chi2=0;for(int b=0;b<256;b++){double d=bins[b]-ex;chi2+=d*d/ex;}
        if(lo>best_ava||(lo==best_ava&&chi2<best_chi2)){best_ava=lo;best_chi2=(int)chi2;bi1=a;bi2=b;bi3=c;bi4=d;}
        if(lo>=39||chi2<240)printf("%6s %6s %6s %6s %5.1f%% %7.1f\n",cn[a],cn[b],cn[c],cn[d],lo,chi2);
    }
    printf("\nBEST: {%s,%s,%s,%s} ava=%.1f%% chi2=%d\n",cn[bi1],cn[bi2],cn[bi3],cn[bi4],best_ava,best_chi2);
    return 0;
}
