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

/* Hash with cross-mix rotation X after main loop */
uint64_t hx(const void*b,size_t l,uint64_t s,int xr){
    const uint8_t*p=b,*e=p+l;uint64_t ba=s^(l*P),h1=ba*P,h2=ba*M1,h3=ba*M2,h4=ba*M3;
    while(p+32<=e){h1^=*(const uint64_t*)p;p+=8;h1=rot(h1+h2,11);h2^=*(const uint64_t*)p;p+=8;h2=rot(h2+h3,17);h3^=*(const uint64_t*)p;p+=8;h3=rot(h3+h4,23);h4^=*(const uint64_t*)p;p+=8;h4=rot(h4+h1,57);}
    while(p+8<=e){h1^=*(const uint64_t*)p;p+=8;h1=rot(h1+h2,11);}
    if(xr>0){h2^=h1;h2=rot(h2,xr);h1^=h2;}  /* cross-mix with rotation xr */
    uint64_t t=0;switch(e-p){case 7:t^=(uint64_t)p[6]<<48;case 6:t^=(uint64_t)p[5]<<40;case 5:t^=(uint64_t)p[4]<<32;case 4:t^=(uint64_t)p[3]<<24;case 3:t^=(uint64_t)p[2]<<16;case 2:t^=(uint64_t)p[1]<<8;case 1:t^=p[0];h1^=t;h1=rot(h1+h2,11);}
    h1^=h2;h3^=h4;h1^=h3;h1^=h1>>29;h1*=M1;h1^=h1>>31;h1*=M2;h1^=h1>>37;h1*=P;h1^=h1>>41;return h1;
}

int main(void){
    printf("Cross-mix rotation scan (xr=0 means no cross-mix = baseline)\n\n");
    printf("%3s %6s %7s\n","xr","ava%","chi2");
    /* baseline: xr=0 */
    {uint8_t buf[256];for(int i=0;i<256;i++)buf[i]=i*0x9D+0x37;uint64_t base=hx(buf,256,0,0);double lo=100;
    for(int by=0;by<32;by++)for(int bi=0;bi<8;bi++){buf[by]^=(1u<<bi);uint64_t hv=hx(buf,256,0,0);buf[by]^=(1u<<bi);double p=pop(base^hv)/64.0*100;if(p<lo)lo=p;}
    int bins[256]={0};for(int i=0;i<256000;i++)bins[hx(&i,4,i,0)&255]++;double ex=1000.0,chi2=0;for(int b=0;b<256;b++){double d=bins[b]-ex;chi2+=d*d/ex;}
    printf("%3d %5.1f%% %7.1f  (baseline)\n",0,lo,chi2);}
    /* scan odd rotations 11-61 */
    for(int xr=11;xr<=61;xr+=2){
        uint8_t buf[256];for(int i=0;i<256;i++)buf[i]=i*0x9D+0x37;uint64_t base=hx(buf,256,0,xr);double lo=100;
        for(int by=0;by<32;by++)for(int bi=0;bi<8;bi++){buf[by]^=(1u<<bi);uint64_t hv=hx(buf,256,0,xr);buf[by]^=(1u<<bi);double p=pop(base^hv)/64.0*100;if(p<lo)lo=p;}
        int bins[256]={0};for(int i=0;i<256000;i++)bins[hx(&i,4,i,xr)&255]++;double ex=1000.0,chi2=0;for(int b=0;b<256;b++){double d=bins[b]-ex;chi2+=d*d/ex;}
        if(lo>=35.0||chi2<250) printf("%3d %5.1f%% %7.1f\n",xr,lo,chi2);
        else if(xr%10==1) printf("%3d %5.1f%% %7.1f (skip)\n",xr,lo,chi2);
    }
    return 0;
}
