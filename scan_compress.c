#include <stdio.h>
#include <stdint.h>
#include <string.h>
static inline uint64_t rot_safe(uint64_t x,int k){return(x<<k)|(x>>(64-k));}
static inline int pop(uint64_t x){x-=x>>1&0x5555555555555555ULL;x=(x&0x3333333333333333ULL)+(x>>2&0x3333333333333333ULL);x=(x+(x>>4))&0x0F0F0F0F0F0F0F0FULL;return(x*0x0101010101010101ULL)>>56;}
#define P 0x9E3779B97F4A7C15ULL
#define M1 0x85EBCA77C2B2AE3DULL
#define M2 0xBF58476D1CE4E5B9ULL
#define M3 0x94D049BB133111EBULL

uint64_t h(const void*b,size_t l,uint64_t s,int r1,int r2){
    const uint8_t*p=b,*e=p+l;uint64_t ba=s^(l*P),h1=ba*P,h2=ba*M1,h3=ba*M2,h4=ba*M3;
    while(p+32<=e){h1^=*(const uint64_t*)p;h1=rot_safe(h1+h2,11);p+=8;h2^=*(const uint64_t*)p;h2=rot_safe(h2+h3,17);p+=8;h3^=*(const uint64_t*)p;h3=rot_safe(h3+h4,23);p+=8;h4^=*(const uint64_t*)p;h4=rot_safe(h4+h1,57);p+=8;}
    while(p+8<=e){h1^=*(const uint64_t*)p;h1=rot_safe(h1+h2,11);p+=8;}
    uint64_t t=0;switch(e-p){case 7:t^=(uint64_t)p[6]<<48;case 6:t^=(uint64_t)p[5]<<40;case 5:t^=(uint64_t)p[4]<<32;case 4:t^=(uint64_t)p[3]<<24;case 3:t^=(uint64_t)p[2]<<16;case 2:t^=(uint64_t)p[1]<<8;case 1:t^=p[0];h1^=t;h1=rot_safe(h1+h2,11);}
    /* Scanned lane compression */
    h1 ^= rot_safe(h2, r1); h3 ^= rot_safe(h4, r2); h1 ^= h3;
    /* Standard finalizer */
    h1^=h1>>29;h1*=M1;h1^=h1>>31;h1*=M2;h1^=h1>>37;h1*=P;h1^=h1>>41;return h1;
}

int main(void){
    int shifts[]={0,11,13,15,17,19,21,23,25,27,29,31,33,35,37,39,41,43,45,47,49,51,53,55,57,59,61};
    int ns=27;
    printf("Lane compression rotation scan (r1,r2):\n");
    printf("r1=0,r2=0 = bare XOR (current baseline)\n\n");
    printf("%3s %3s %6s %7s\n","r1","r2","ava%","chi2");
    
    double best_ava=0; int best_chi2=999,br1=0,br2=0;
    /* Baseline */
    {uint8_t buf[256];for(int i=0;i<256;i++)buf[i]=i*0x9D+0x37;uint64_t base=h(buf,256,0,0,0);double lo=100;
    for(int by=0;by<32;by++)for(int bi=0;bi<8;bi++){buf[by]^=(1u<<bi);uint64_t hv=h(buf,256,0,0,0);buf[by]^=(1u<<bi);double p=pop(base^hv)/64.0*100;if(p<lo)lo=p;}
    int bins[256]={0};for(int i=0;i<256000;i++)bins[h(&i,4,i,0,0)&255]++;double ex=1000.0,chi2=0;for(int b=0;b<256;b++){double d=bins[b]-ex;chi2+=d*d/ex;}
    printf("%3d %3d %5.1f%% %7.1f  (bare XOR baseline)\n",0,0,lo,chi2);best_ava=lo;best_chi2=(int)chi2;}
    
    for(int a=0;a<ns;a++){int r1=shifts[a];
    for(int b=0;b<ns;b++){int r2=shifts[b];if(r1==0&&r2==0)continue;
    uint8_t buf[256];for(int i=0;i<256;i++)buf[i]=i*0x9D+0x37;uint64_t base=h(buf,256,0,r1,r2);double lo=100;int fail=0;
    for(int by=0;by<32&&!fail;by++)for(int bi=0;bi<8&&!fail;bi++){buf[by]^=(1u<<bi);uint64_t hv=h(buf,256,0,r1,r2);buf[by]^=(1u<<bi);double p=pop(base^hv)/64.0*100;if(p<lo)lo=p;if(p<25)fail=1;}
    if(fail)continue;
    int bins[256]={0};for(int i=0;i<64000;i++)bins[h(&i,4,i,r1,r2)&255]++;double ex2=250.0,chi2v=0;for(int i=0;i<256;i++){double d=bins[i]-ex2;chi2v+=d*d/ex2;}
    if(lo>best_ava||(lo==best_ava&&chi2v<best_chi2)){best_ava=lo;best_chi2=(int)chi2v;br1=r1;br2=r2;}
    if(lo>=39&&chi2v<240)printf("%3d %3d %5.1f%% %7.1f\n",r1,r2,lo,chi2v);
    }}
    printf("\nBEST: r1=%d r2=%d ava=%.1f%% chi2=%d\n",br1,br2,best_ava,best_chi2);
    return 0;
}
