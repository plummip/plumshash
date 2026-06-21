#include <stdio.h>
#include <stdint.h>
#include <string.h>
static inline uint64_t rot(uint64_t x,int k){return(x<<k)|(x>>(64-k));}
#define VP 0x9E3779B97F4A7C15ULL
#define VM1 0x85EBCA77C2B2AE3DULL
#define VM2 0xBF58476D1CE4E5B9ULL

/* ARX body + finalizer with given shifts */
uint64_t h(const void*b,size_t l,uint64_t s,int s1,int s2,int s3,int s4){
    const uint8_t*p=b,*e=p+l;uint64_t ba=s^(l*VP),h1=ba*VP,h2=ba*VM1,h3=ba*VM2,h4=ba*VM1;
    while(p+32<=e){h1^=*(const uint64_t*)p;h1=rot(h1+h2,11);p+=8;h2^=*(const uint64_t*)p;h2=rot(h2+h3,17);p+=8;h3^=*(const uint64_t*)p;h3=rot(h3+h4,23);p+=8;h4^=*(const uint64_t*)p;h4=rot(h4+h1,57);p+=8;}
    while(p+8<=e){h1^=*(const uint64_t*)p;h1=rot(h1+h2,11);p+=8;}
    uint64_t t=0;switch(e-p){case 7:t^=(uint64_t)p[6]<<48;case 6:t^=(uint64_t)p[5]<<40;case 5:t^=(uint64_t)p[4]<<32;case 4:t^=(uint64_t)p[3]<<24;case 3:t^=(uint64_t)p[2]<<16;case 2:t^=(uint64_t)p[1]<<8;case 1:t^=p[0];h1^=t;h1=rot(h1+h2,11);}
    h1^=h2;h3^=h4;h1^=h3;
    h1^=h1>>s1;h1*=VM1;h1^=h1>>s2;h1*=VM2;h1^=h1>>s3;h1*=VP;h1^=h1>>s4;
    return h1;
}

static int pop(uint64_t x){x-=x>>1&0x5555555555555555ULL;x=(x&0x3333333333333333ULL)+(x>>2&0x3333333333333333ULL);x=(x+(x>>4))&0x0F0F0F0F0F0F0F0FULL;return(x*0x0101010101010101ULL)>>56;}

int main(void){
    printf("Finalizer shift scan (target: chi2 < 247, ava > 35%%)\n\n");
    int shifts[]={17,19,23,27,29,31,33,37,39,41,43,47,53};
    int ns=sizeof(shifts)/sizeof(shifts[0]);
    int best_chi2=999;int best_s1=0,best_s2=0,best_s3=0,best_s4=0;double best_ava=0;

    for(int a=0;a<ns;a++)for(int b=0;b<ns;b++)for(int c=0;c<ns;c++)for(int d=0;d<ns;d++){
        int s1=shifts[a],s2=shifts[b],s3=shifts[c],s4=shifts[d];
        if(s1==s2||s1==s3||s1==s4||s2==s3||s2==s4||s3==s4)continue;
        /* chi2 test */
        int bins[256]={0};for(int i=0;i<64000;i++)bins[h(&i,4,i,s1,s2,s3,s4)&255]++;
        double ex=250.0,chi2=0;for(int i=0;i<256;i++){double d=bins[i]-ex;chi2+=d*d/ex;}
        if(chi2>350)continue;
        /* avalanche quick check */
        uint8_t buf[256];for(int i=0;i<256;i++)buf[i]=i*0x9D+0x37;
        uint64_t base=h(buf,256,0,s1,s2,s3,s4);double lo=100;int fail=0;
        for(int by=0;by<8&&!fail;by++)for(int bi=0;bi<8&&!fail;bi++){buf[by]^=(1u<<bi);uint64_t hv=h(buf,256,0,s1,s2,s3,s4);buf[by]^=(1u<<bi);double p=pop(base^hv)/64.0*100;if(p<lo)lo=p;if(p<25)fail=1;}
        if(fail)continue;
        if(chi2<best_chi2||(chi2==best_chi2&&lo>best_ava)){best_chi2=(int)chi2;best_ava=lo;best_s1=s1;best_s2=s2;best_s3=s3;best_s4=s4;}
        if(chi2<260&&lo>=35)printf("{%2d,%2d,%2d,%2d} chi2=%.0f ava=%.1f%%\n",s1,s2,s3,s4,chi2,lo);
    }
    printf("\nBEST: {%d,%d,%d,%d} chi2=%d ava=%.1f%%\n",best_s1,best_s2,best_s3,best_s4,best_chi2,best_ava);
    printf("Current {29,31,37,41} — testing...\n");
    int bins[256]={0};for(int i=0;i<256000;i++)bins[h(&i,4,i,29,31,37,41)&255]++;
    double ex=1000.0,chi2=0;for(int i=0;i<256;i++){double d=bins[i]-ex;chi2+=d*d/ex;}
    printf("Current chi2=%.1f\n",chi2);
    return 0;
}
