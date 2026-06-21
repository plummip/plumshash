#define VORTEXHASH_IMPLEMENTATION
#include "vortexhash.h"
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

static int pop(uint64_t x){x-=x>>1&0x5555555555555555ULL;x=(x&0x3333333333333333ULL)+(x>>2&0x3333333333333333ULL);x=(x+(x>>4))&0x0F0F0F0F0F0F0F0FULL;return(x*0x0101010101010101ULL)>>56;}

int main(void) {
    printf("VortexHash — verification\n=========================\n\n");
    printf("Sanity:\n");
    uint64_t a = vortexhash("", 0, 0);
    printf("  deterministic: %s\n", a == vortexhash("", 0, 0) ? "PASS" : "FAIL");
    printf("  nonzero output: %s\n", vortexhash("hello", 5, 0) != 0 ? "PASS" : "FAIL");
    printf("  seeded: %s\n", vortexhash("",0,0) != vortexhash("",0,42) ? "PASS" : "FAIL");
    printf("  diff input: %s\n", vortexhash("abc",3,0) != vortexhash("abd",3,0) ? "PASS" : "FAIL");

    uint8_t buf[256];
    for(int i=0;i<256;i++)buf[i]=i*0x9D+0x37;
    uint64_t base=vortexhash(buf,256,0);
    double lo=100,sum=0;
    for(int by=0;by<32;by++)for(int bi=0;bi<8;bi++){
        buf[by]^=(1u<<bi);uint64_t hv=vortexhash(buf,256,0);buf[by]^=(1u<<bi);
        double p=pop(base^hv)/64.0*100;sum+=p;if(p<lo)lo=p;
    }
    printf("\nAvalanche: avg=%.1f%% worst=%.1f%%\n",sum/256,lo);

    int bins[256]={0};
    for(int i=0;i<256000;i++)bins[vortexhash(&i,4,i)&255]++;
    double ex=1000.0,chi2=0;
    for(int b=0;b<256;b++){double d=bins[b]-ex;chi2+=d*d/ex;}
    printf("Chi2: %.1f (df=255, p95=293)\n",chi2);

    printf("\nSpeed (GB/s):\n");
    size_t szs[]={64,256,1024,8192,65536};
    for(int si=0;si<5;si++){size_t sz=szs[si];uint8_t*b=malloc(sz);
        for(size_t i=0;i<sz;i++)b[i]=i*0x9D+0x37;
        int it=sz<1024?50000000/sz:500000;if(it<10)it=10;
        volatile uint64_t s=0;clock_t st=clock();
        for(int i=0;i<it;i++)s^=vortexhash(b,sz,i);
        clock_t en=clock();
        printf("  %5zuB: %6.1f GB/s\n",sz,sz*(double)it/((double)(en-st)/CLOCKS_PER_SEC)/1e9);
        free(b);
    }

    printf("\nSingle-header, ~100 LOC.  #include \"vortexhash.h\"\n");
    printf("Then #define VORTEXHASH_IMPLEMENTATION before the include in ONE .c file.\n");
    return 0;
}
