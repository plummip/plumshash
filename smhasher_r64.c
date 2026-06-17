/* SMHasher-grade verification for R64 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include <stdint.h>

#define R64_IMPLEMENTATION
#include "r64.h"

static inline int popcount(uint64_t x) {
    x -= (x >> 1) & 0x5555555555555555ULL;
    x  = (x & 0x3333333333333333ULL) + ((x >> 2) & 0x3333333333333333ULL);
    x  = (x + (x >> 4)) & 0x0F0F0F0F0F0F0F0FULL;
    return (int)((x * 0x0101010101010101ULL) >> 56);
}

static int pass = 0, fail = 0;
#define TEST(name, ok) do { if (ok) pass++; else { fail++; printf("  FAIL: %s\n", name); } } while(0)

int main(void) {
    printf("R64 SMHasher-grade verification\n");
    printf("================================\n\n");

    /* Sanity */
    printf("── Sanity ──\n");
    TEST("deterministic", r64hash("hello",5,42) == r64hash("hello",5,42));
    TEST("seed_diff", r64hash("hello",5,0) != r64hash("hello",5,1));
    TEST("key_diff", r64hash("hello",5,0) != r64hash("world",5,0));
    TEST("empty_key", r64hash("",0,42) != 0);
    TEST("len_matters", r64hash("abc",3,0) != r64hash("abc\0",4,0));

    /* Avalanche */
    printf("── Avalanche ──\n");
    {
        uint8_t buf[256];
        for(int i=0;i<256;i++) buf[i]=(uint8_t)(i*0x9D+0x37);
        double worst=100;
        for(int by=0;by<32;by++)for(int bi=0;bi<8;bi++){
            buf[by]^=(1u<<bi);
            uint64_t h0=r64hash(buf,256,0);
            buf[by]^=(1u<<bi);
            uint64_t h1=r64hash(buf,256,0);
            double p=popcount(h0^h1)/64.0*100;
            if(p<worst)worst=p;
        }
        printf("  fast (256B): worst=%.1f%%\n",worst);
        TEST("avalanche_fast",worst>=30.0);
    }
    {
        uint8_t buf[32];
        for(int i=0;i<32;i++) buf[i]=(uint8_t)(i*0x9D+0x37);
        double worst=100;
        for(int by=0;by<32;by++)for(int bi=0;bi<8;bi++){
            buf[by]^=(1u<<bi);
            uint64_t h0=r64hash(buf,32,0);
            buf[by]^=(1u<<bi);
            uint64_t h1=r64hash(buf,32,0);
            double p=popcount(h0^h1)/64.0*100;
            if(p<worst)worst=p;
        }
        printf("  short (32B): worst=%.1f%%\n",worst);
        TEST("avalanche_short",worst>=30.0);
    }

    /* Differential */
    printf("── Differential ──\n");
    {
        uint8_t buf[64];
        for(int i=0;i<64;i++) buf[i]=(uint8_t)i;
        double sum=0; int n=512;
        for(int i=0;i<n;i++){
            uint64_t h0=r64hash(buf,64,i);
            uint64_t h1=r64hash(buf,64,i+1);
            sum+=popcount(h0^h1);
        }
        printf("  avg diff bits (seed+1): %.1f / 64\n",sum/n);
        TEST("differential_seed",sum/n>25&&sum/n<40);
    }

    /* Chi2 */
    printf("── chi² ──\n");
    {
        int bins[256]={0};
        for(int i=0;i<256000;i++) bins[r64hash(&i,sizeof(i),i)&0xFF]++;
        double ex=1000.0, chi2=0;
        for(int b=0;b<256;b++){double d=bins[b]-ex;chi2+=d*d/ex;}
        printf("  χ² = %.1f (<300 pass)\n",chi2);
        TEST("chi2",chi2<300);
    }

    /* Sparse */
    printf("── Sparse ──\n");
    {
        uint64_t *seen=calloc(20000,sizeof(uint64_t));
        int ngen=0; uint8_t key[256];
        for(int pos=0;pos<128&&ngen<20000;pos++)
            for(int val=1;val<256&&ngen<20000;val++){
                int klen=8+(pos%57);
                memset(key,0,klen); key[klen-1]=(uint8_t)val;
                if(pos&1) key[0]=(uint8_t)(pos^val);
                seen[ngen++]=r64hash(key,klen,0);
            }
        int collisions=0;
        for(int i=0;i<ngen;i++)for(int j=i+1;j<ngen;j++)if(seen[i]==seen[j])collisions++;
        printf("  collisions: %d / %d\n",collisions,ngen);
        TEST("sparse",collisions<=30);
        free(seen);
    }

    /* Permutation */
    printf("── Permutation ──\n");
    {
        uint8_t buf[32];
        for(int i=0;i<32;i++) buf[i]=(uint8_t)(i*7+3);
        uint64_t base=r64hash(buf,32,0);
        int diff=0;
        for(int i=0;i<31;i++){
            uint8_t t=buf[i];buf[i]=buf[i+1];buf[i+1]=t;
            if(r64hash(buf,32,0)!=base)diff++;
            buf[i+1]=buf[i];buf[i]=t;
        }
        printf("  different after swap: %d / 31\n",diff);
        TEST("permutation",diff>=30);
    }

    /* AppendedZeroes */
    printf("── AppendedZeroes ──\n");
    {
        const char *key="test";
        uint64_t h0=r64hash(key,4,0); uint8_t buf[32];
        memset(buf,0,32); memcpy(buf,key,4);
        int diff=0;
        for(int i=5;i<=20;i++) if(r64hash(buf,i,0)!=h0) diff++;
        printf("  different with appended zeros: %d / 16\n",diff);
        TEST("appended_zeroes",diff>=15);
    }

    /* Speed */
    printf("── Speed ──\n");
    {
        uint8_t *buf=malloc(4096); memset(buf,0xAB,4096);
        const int iters=2000000;
        struct{int len;const char*l;}sz[]={{4,"4B"},{16,"16B"},{64,"64B"},{256,"256B"},{1024,"1KB"},{4096,"4KB"},{0,NULL}};
        for(int si=0;sz[si].l;si++){
            int len=sz[si].len,n=(len<64)?iters*4:iters;
            uint64_t acc=0;acc+=r64hash(buf,len,0);__asm__("":"+r"(acc));
            struct timespec t0,t1;
            clock_gettime(CLOCK_MONOTONIC,&t0);
            for(int i=0;i<n;i++)acc+=r64hash(buf,len,acc^i);
            clock_gettime(CLOCK_MONOTONIC,&t1);__asm__("":"+r"(acc));
            double sec=(t1.tv_sec-t0.tv_sec)+(t1.tv_nsec-t0.tv_nsec)*1e-9;
            printf("  %-5s %7.2f GB/s\n",sz[si].l,((double)len*n/1e9)/sec);
        }
        free(buf);
    }

    printf("\n%d passed, %d failed\n",pass,fail);
    return fail?1:0;
}
