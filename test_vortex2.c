#define VORTEXHASH_IMPLEMENTATION
#include "vortexhash.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>

static inline int pop(uint64_t x){x-=x>>1&0x5555555555555555ULL;x=(x&0x3333333333333333ULL)+(x>>2&0x3333333333333333ULL);x=(x+(x>>4))&0x0F0F0F0F0F0F0F0FULL;return(x*0x0101010101010101ULL)>>56;}

static int tr,tf;static const char*ct;
#define T(n) do{ct=n;tr++;}while(0)
#define F(f,...) do{fprintf(stderr,"  FAIL [%s] " f "\n",ct,##__VA_ARGS__);tf++;return;}while(0)

void test_ava(void){
    T("avalanche");uint8_t b[256];for(int i=0;i<256;i++)b[i]=i*0x9D+0x37;
    uint64_t base=vortexhash(b,256,0);double s=0,lo=100,hi=0;
    for(int by=0;by<32;by++)for(int bi=0;bi<8;bi++){b[by]^=(1u<<bi);uint64_t hv=vortexhash(b,256,0);b[by]^=(1u<<bi);
        double p=pop(base^hv)/64.0*100;s+=p;if(p<lo)lo=p;if(p>hi)hi=p;}
    printf("  ava: avg=%.1f%% worst=%.1f%% best=%.1f%%\n",s/256,lo,hi);
    if(s/256<45||s/256>55)F("avg %.1f",s/256);if(lo<33)F("worst %.1f",lo);
}

void test_dist(void){
    T("dist");int bins[256]={0};
    for(int i=0;i<256000;i++)bins[vortexhash(&i,4,i)&255]++;
    double ex=1000.0,chi2=0;
    for(int b=0;b<256;b++){double d=bins[b]-ex;chi2+=d*d/ex;}
    printf("  chi2: %.1f\n",chi2);if(chi2>350)F("chi2 %.1f",chi2);
}

void test_sparse(void){
    T("sparse");int cols=0;uint64_t seen[20000]={0};int ngen=0;uint8_t key[256];
    for(int pos=0;pos<128&&ngen<20000;pos++)for(int val=1;val<256&&ngen<20000;val++){
        int kl=8+(pos%57);memset(key,0,kl);key[kl-1]=val;if(pos&1)key[0]=pos^val;
        seen[ngen++]=vortexhash(key,kl,0);}
    for(int i=0;i<ngen;i++)for(int j=i+1;j<ngen;j++)if(seen[i]==seen[j])cols++;
    printf("  sparse: %d collisions / %d keys\n",cols,ngen);
    if(cols>30)F("%d collisions",cols);
}

void test_speed(void){
    T("speed");size_t szs[]={64,256,1024,8192,65536,262144};int ns=6;
    printf("  speed:");
    for(int si=0;si<ns;si++){size_t sz=szs[si];uint8_t*b=malloc(sz);
        for(size_t i=0;i<sz;i++)b[i]=i*0x9D+0x37;
        int it=sz<1024?50000000/sz:500000;if(it<10)it=10;
        volatile uint64_t s=0;clock_t st=clock();
        for(int i=0;i<it;i++)s^=vortexhash(b,sz,i);clock_t en=clock();
        printf(" %zu:%.1f",sz,sz*(double)it/((double)(en-st)/CLOCKS_PER_SEC)/1e9);free(b);}
    printf(" GB/s\n");
}

int main(void){
    printf("=== VortexHash 2-Level State ===\n\n");
    test_ava();
    test_dist();
    test_sparse();
    test_speed();
    printf("\nResult: %d/%d passed\n",tr-tf,tr);
    return tf>0?1:0;
}
