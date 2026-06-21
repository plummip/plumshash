/*
 * scan_final_full.c — Exhaustive finalizer shift scan (all odd 11-61)
 * ===================================================================
 * Tests 26^4 = 456,976 shift combinations.
 * Prunes early: skip duplicates, reject chi2>350, reject ava<33.
 * Reports best chi2 with ava>=35%.
 */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <time.h>

static inline uint64_t rot(uint64_t x,int k){return(x<<k)|(x>>(64-k));}
static inline int pop(uint64_t x){x-=x>>1&0x5555555555555555ULL;x=(x&0x3333333333333333ULL)+(x>>2&0x3333333333333333ULL);x=(x+(x>>4))&0x0F0F0F0F0F0F0F0FULL;return(x*0x0101010101010101ULL)>>56;}
#define P 0x9E3779B97F4A7C15ULL
#define M1 0x85EBCA77C2B2AE3DULL
#define M2 0xBF58476D1CE4E5B9ULL

uint64_t h(const void*b,size_t l,uint64_t s,int s1,int s2,int s3,int s4){
    const uint8_t*p=b,*e=p+l;uint64_t ba=s^(l*P),h1=ba*P,h2=ba*M1,h3=ba*M2,h4=ba*M1;
    while(p+32<=e){h1^=*(const uint64_t*)p;h1=rot(h1+h2,11);p+=8;h2^=*(const uint64_t*)p;h2=rot(h2+h3,17);p+=8;h3^=*(const uint64_t*)p;h3=rot(h3+h4,23);p+=8;h4^=*(const uint64_t*)p;h4=rot(h4+h1,57);p+=8;}
    while(p+8<=e){h1^=*(const uint64_t*)p;h1=rot(h1+h2,11);p+=8;}
    uint64_t t=0;switch(e-p){case 7:t^=(uint64_t)p[6]<<48;case 6:t^=(uint64_t)p[5]<<40;case 5:t^=(uint64_t)p[4]<<32;case 4:t^=(uint64_t)p[3]<<24;case 3:t^=(uint64_t)p[2]<<16;case 2:t^=(uint64_t)p[1]<<8;case 1:t^=p[0];h1^=t;h1=rot(h1+h2,11);}
    h1^=h2;h3^=h4;h1^=h3;h1^=h1>>s1;h1*=M1;h1^=h1>>s2;h1*=M2;h1^=h1>>s3;h1*=P;h1^=h1>>s4;return h1;
}

int main(void){
    int shifts[]={11,13,15,17,19,21,23,25,27,29,31,33,35,37,39,41,43,45,47,49,51,53,55,57,59,61};
    int ns=26;
    time_t start=time(NULL);
    int tested=0,passed=0;
    int best_chi2=999,best_s1=0,best_s2=0,best_s3=0,best_s4=0;double best_ava=0;
    
    printf("Exhaustive finalizer shift scan: 26^4 = %d combos\n",ns*ns*ns*ns);
    printf("Target: chi2<240, ava>=35%%\n\n");
    
    for(int a=0;a<ns;a++){int s1=shifts[a];
    for(int b=0;b<ns;b++){int s2=shifts[b];
    if(s2==s1)continue;
    for(int c=0;c<ns;c++){int s3=shifts[c];
    if(s3==s1||s3==s2)continue;
    for(int d=0;d<ns;d++){int s4=shifts[d];
    if(s4==s1||s4==s2||s4==s3)continue;
    tested++;
    
    /* Quick chi2: 64K samples */
    int bins[256]={0};for(int i=0;i<64000;i++)bins[h(&i,4,i,s1,s2,s3,s4)&255]++;
    double ex=250.0,chi2=0;for(int i=0;i<256;i++){double d=bins[i]-ex;chi2+=d*d/ex;}
    if(chi2>300)continue; /* quick reject */
    
    /* Full avalanche */
    uint8_t buf[256];for(int i=0;i<256;i++)buf[i]=i*0x9D+0x37;
    uint64_t base=h(buf,256,0,s1,s2,s3,s4);double lo=100;int fail=0;
    for(int by=0;by<32&&!fail;by++)for(int bi=0;bi<8&&!fail;bi++){buf[by]^=(1u<<bi);uint64_t hv=h(buf,256,0,s1,s2,s3,s4);buf[by]^=(1u<<bi);double p=pop(base^hv)/64.0*100;if(p<lo)lo=p;if(p<25)fail=1;}
    if(fail)continue;
    
    passed++;
    if(chi2<best_chi2||(chi2==best_chi2&&lo>best_ava)){best_chi2=(int)chi2;best_ava=lo;best_s1=s1;best_s2=s2;best_s3=s3;best_s4=s4;}
    if(chi2<240&&lo>=35)printf("{%2d,%2d,%2d,%2d} chi2=%.0f ava=%.1f%%\n",s1,s2,s3,s4,chi2,lo);
    }}}}
    
    printf("\nTested: %d  Passed: %d  Time: %lds\n",tested,passed,time(NULL)-start);
    printf("BEST: {%d,%d,%d,%d} chi2=%d ava=%.1f%%\n",best_s1,best_s2,best_s3,best_s4,best_chi2,best_ava);
    printf("Current {29,31,37,41} — quick test:\n");
    int bins[256]={0};for(int i=0;i<256000;i++)bins[h(&i,4,i,29,31,37,41)&255]++;
    double ex2=1000.0,chi2_cur=0;for(int i=0;i<256;i++){double d=bins[i]-ex2;chi2_cur+=d*d/ex2;}
    printf("Current chi2=%.1f\n",chi2_cur);
    return 0;
}
