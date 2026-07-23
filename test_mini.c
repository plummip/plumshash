#define PLUMSHASH_MINI_IMPLEMENTATION
#include "plumshash_mini.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>

static inline int pop(uint64_t x) {
    x-=(x>>1)&0x5555555555555555ULL;
    x=(x&0x3333333333333333ULL)+((x>>2)&0x3333333333333333ULL);
    x=(x+(x>>4))&0x0F0F0F0F0F0F0F0FULL;
    return (int)((x*0x0101010101010101ULL)>>56);
}

int main(void) {
    int pass=0,fail=0;
    #define T(n,ok) do{if(ok)pass++;else{printf("FAIL: %s\n",n);fail++;}}while(0)

    /* Sanity */
    T("det", plumshash_mini("hello",5,42)==plumshash_mini("hello",5,42));
    T("seed", plumshash_mini("hello",5,0)!=plumshash_mini("hello",5,1));
    T("key",  plumshash_mini("hello",5,0)!=plumshash_mini("world",5,0));

    /* Avalanche 64B */
    uint8_t buf[64]; memset(buf,0xA5,64);
    double worst=100;
    for(int by=0;by<32;by++)for(int bi=0;bi<8;bi++){
        buf[by]^=(uint8_t)(1u<<bi);
        uint64_t h0=plumshash_mini(buf,64,42+by*8+bi);
        buf[by]^=(uint8_t)(1u<<bi);
        uint64_t h1=plumshash_mini(buf,64,42+by*8+bi);
        double p=pop(h0^h1)/64.0*100;
        if(p<worst)worst=p;
    }
    printf("Avalanche (64B): %.1f%%\n",worst);
    T("avalanche",worst>=25.0);

    /* Chi2 */
    int bins[256]={0};
    for(int i=0;i<100000;i++){uint64_t h=plumshash_mini(&i,sizeof(i),i);bins[h&0xFF]++;}
    double chi2=0,exp=100000.0/256;
    for(int b=0;b<256;b++){double d=bins[b]-exp;chi2+=d*d/exp;}
    printf("Chi2: %.1f\n",chi2);
    T("chi2",chi2<300);

    /* Speed */
    const int N=2000000;
    uint8_t *kb=(uint8_t*)malloc(4096);
    memset(kb,0xAB,4096);
    uint64_t acc=plumshash_mini(kb,4096,0);
    struct timespec t0,t1;
    clock_gettime(CLOCK_MONOTONIC,&t0);
    for(int i=0;i<N;i++)acc+=plumshash_mini(kb,4096,acc^(uint64_t)i);
    clock_gettime(CLOCK_MONOTONIC,&t1);
    double sec=(t1.tv_sec-t0.tv_sec)+(t1.tv_nsec-t0.tv_nsec)*1e-9;
    printf("Speed (4KB): %.2f GB/s\n",(4096.0*N/1e9)/sec);
    free(kb);

    printf("\n%d passed, %d failed\n",pass,fail);
    return fail?1:0;
}
