#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <math.h>
static inline uint64_t rot(uint64_t x,int k){return(x<<k)|(x>>(64-k));}
static inline int pop(uint64_t x){x-=x>>1&0x5555555555555555ULL;x=(x&0x3333333333333333ULL)+(x>>2&0x3333333333333333ULL);x=(x+(x>>4))&0x0F0F0F0F0F0F0F0FULL;return(x*0x0101010101010101ULL)>>56;}
#define P 0x9E3779B97F4A7C15ULL
#define M1 0x85EBCA77C2B2AE3DULL
#define M2 0xBF58476D1CE4E5B9ULL
#define M3 0x94D049BB133111EBULL

/* Safe path with tunable accumulator rotation and multiplier */
uint64_t hs(const void*b,size_t l,uint64_t s,int acc_rot,uint64_t acc_mul){
    const uint8_t*p=b,*e=p+l;uint64_t ba=s^(l*P),h1=ba*P,h2=ba*M1,h3=ba*M2,h4=ba*M3,acc=ba^M2;int hb=0;
    while(p+32<=e){uint64_t v1=*(const uint64_t*)p;p+=8;uint64_t v2=*(const uint64_t*)p;p+=8;uint64_t v3=*(const uint64_t*)p;p+=8;uint64_t v4=*(const uint64_t*)p;p+=8;
        h1^=v1;h1=rot(h1+h2,11);h2^=v2;h2=rot(h2+h3,17);h3^=v3;h3=rot(h3+h4,23);h4^=v4;h4=rot(h4+h1,57);acc^=v1^v2^v3^v4;acc=rot(acc,acc_rot);acc*=acc_mul;hb=1;}
    while(p+8<=e){uint64_t v=*(const uint64_t*)p;p+=8;h1^=v;h1=rot(h1+h2,11);acc^=v;acc=rot(acc,acc_rot);acc*=acc_mul;hb=1;}
    h2^=h1;h2=rot(h2,43);h1^=h2;
    uint64_t t=0;switch(e-p){case 7:t^=(uint64_t)p[6]<<48;case 6:t^=(uint64_t)p[5]<<40;case 5:t^=(uint64_t)p[4]<<32;case 4:t^=(uint64_t)p[3]<<24;case 3:t^=(uint64_t)p[2]<<16;case 2:t^=(uint64_t)p[1]<<8;case 1:t^=p[0];}if(t){h1^=t;h1=rot(h1+h2,11);if(hb){acc^=t;acc=rot(acc,acc_rot);acc*=acc_mul;}}
    h1^=h2;h3^=h4;h1^=h3;if(hb)h1^=acc;h1^=h1>>29;h1*=M1;h1^=h1>>31;h1*=M2;h1^=h1>>37;h1*=P;h1^=h1>>41;return h1;
}

int main(void){
    /* Scan accumulator rotation (odd 11-61) with current multiplier PHI */
    printf("Accumulator rotation scan (multiplier=PHI, cross-mix=43):\n");
    printf("%4s %6s %7s\n","rot","ava%","chi2");
    int best_r=31; double best_chi2=999, best_ava=0;
    for(int ar=11;ar<=61;ar+=2){
        uint8_t buf[256];for(int i=0;i<256;i++)buf[i]=i*0x9D+0x37;uint64_t base=hs(buf,256,0,ar,P);double lo=100;
        for(int by=0;by<32;by++)for(int bi=0;bi<8;bi++){buf[by]^=(1u<<bi);uint64_t hv=hs(buf,256,0,ar,P);buf[by]^=(1u<<bi);double p=pop(base^hv)/64.0*100;if(p<lo)lo=p;}
        int bins[256]={0};for(int i=0;i<256000;i++)bins[hs(&i,4,i,ar,P)&255]++;double ex=1000.0,chi2=0;for(int b=0;b<256;b++){double d=bins[b]-ex;chi2+=d*d/ex;}
        if(lo>=33&&chi2<best_chi2){best_chi2=chi2;best_ava=lo;best_r=ar;}
        if(chi2<230||lo>=39) printf("%4d %5.1f%% %7.1f\n",ar,lo,chi2);
    }
    printf("BEST rot=%d chi2=%.1f ava=%.1f%%\n\n",best_r,best_chi2,best_ava);

    /* Now scan accumulator multiplier with best rotation */
    uint64_t multipliers[]={0x9E3779B97F4A7C15ULL,0x85EBCA77C2B2AE3DULL,0xBF58476D1CE4E5B9ULL,0x94D049BB133111EBULL,0xA3C8B9D5E1F2A7B4ULL,0xC6A4A7935BD1E995ULL};
    int nm=6; const char*mn[]={"PHI","M1","M2","M3","M4","M5"};
    printf("Accumulator multiplier scan (rot=%d, cross-mix=43):\n",best_r);
    printf("%4s %6s %7s\n","mul","ava%","chi2");
    int best_m=0; best_chi2=999;
    for(int mi=0;mi<nm;mi++){
        uint64_t mul=multipliers[mi];
        uint8_t buf[256];for(int i=0;i<256;i++)buf[i]=i*0x9D+0x37;uint64_t base=hs(buf,256,0,best_r,mul);double lo=100;
        for(int by=0;by<32;by++)for(int bi=0;bi<8;bi++){buf[by]^=(1u<<bi);uint64_t hv=hs(buf,256,0,best_r,mul);buf[by]^=(1u<<bi);double p=pop(base^hv)/64.0*100;if(p<lo)lo=p;}
        int bins[256]={0};for(int i=0;i<256000;i++)bins[hs(&i,4,i,best_r,mul)&255]++;double ex=1000.0,chi2=0;for(int b=0;b<256;b++){double d=bins[b]-ex;chi2+=d*d/ex;}
        if(lo>=33&&chi2<best_chi2){best_chi2=chi2;best_m=mi;}
        printf("%4s %5.1f%% %7.1f\n",mn[mi],lo,chi2);
    }
    printf("BEST mul=%s chi2=%.1f\n",mn[best_m],best_chi2);
    return 0;
}
