#define PLUMSHASH_IMPLEMENTATION
#include "plumshash.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>

static inline int pop(uint64_t x){
    x-=(x>>1)&0x5555555555555555ULL;
    x=(x&0x3333333333333333ULL)+((x>>2)&0x3333333333333333ULL);
    x=(x+(x>>4))&0x0F0F0F0F0F0F0F0FULL;
    return(int)((x*0x0101010101010101ULL)>>56);
}

static double sparse_chi2(int len, uint64_t seed, int N) {
    uint8_t *key=(uint8_t*)calloc((size_t)len,1);
    int *buckets=(int*)calloc(65536,sizeof(int));
    if(!key||!buckets){free(key);free(buckets);return-1;}
    for(int i=0;i<N;i++){
        key[len-2]=(uint8_t)(i&0xFF);
        key[len-1]=(uint8_t)((i>>8)&0xFF);
        buckets[plumshash(key,(size_t)len,seed)&0xFFFF]++;
    }
    double ex=(double)N/65536,chi2=0;
    for(int b=0;b<65536;b++){double d=buckets[b]-ex;chi2+=d*d/ex;}
    free(key);free(buckets);
    return chi2;
}

int main(void) {
    int pass=0,fail=0,warn=0;
    #define T(n,ok) do{if(ok)pass++;else{printf("FAIL: %s\n",n);fail++;}}while(0)
    #define W(n,ok) do{if(ok)pass++;else{printf("WARN: %s\n",n);warn++;}}while(0)

    printf("══════════════════════════════════════════════\n");
    printf("PLUMSHASH EXTENSIVE TEST SUITE\n");
    printf("══════════════════════════════════════════════\n\n");

    /* ══════ 1. SANITY ══════ */
    printf("─── 1. Sanity ───\n");
    uint64_t h=plumshash("hello",5,42);
    T("deterministic",h==plumshash("hello",5,42));
    T("seed_diff",plumshash("hello",5,0)!=plumshash("hello",5,1));
    T("key_diff",plumshash("hello",5,0)!=plumshash("world",5,0));
    T("empty_key",plumshash("",0,42)!=0);
    T("null_zero_len",plumshash(NULL,0,42)!=0);
    uint8_t a[4]={1,2,3,0},b[3]={1,2,3};
    T("len_matters",plumshash(a,4,0)!=plumshash(b,3,0));
    printf("  %d/%d\n\n",pass,pass+fail+warn);

    /* ══════ 2. AVALANCHE — ALL PATHS ══════ */
    printf("─── 2. Avalanche (all 4 paths) ───\n");
    struct{int len;const char*n;int tb;}paths[]={
        {16,"tiny",16},{32,"safe",32},{48,"medium",32},
        {64,"medium",32},{128,"fast",64},{256,"fast",64},{4096,"fast",64},{0,NULL,0}};
    uint8_t buf[4096];memset(buf,0xA5,4096);
    double overall_worst=100;
    for(int pi=0;paths[pi].n;pi++){
        double worst=100;
        for(int by=0;by<paths[pi].tb;by++)for(int bi=0;bi<8;bi++){
            buf[by]^=(uint8_t)(1u<<bi);
            h=plumshash(buf,paths[pi].len,42+by*8+bi);
            buf[by]^=(uint8_t)(1u<<bi);
            uint64_t h2=plumshash(buf,paths[pi].len,42+by*8+bi);
            double p=pop(h^h2)/64.0*100;
            if(p<worst)worst=p;
        }
        printf("  %-8s (%4dB): worst=%.1f%%\n",paths[pi].n,paths[pi].len,worst);
        if(worst<25.0)fail++;else pass++;
        if(worst<overall_worst)overall_worst=worst;
    }
    printf("  Overall worst: %.1f%%\n\n",overall_worst);

    /* ══════ 3. CHI-SQUARED ══════ */
    printf("─── 3. Chi-squared ───\n");
    int bins[256]={0};
    for(int i=0;i<256000;i++){bins[plumshash(&i,sizeof(i),i)&0xFF]++;}
    double chi2=0,ex=256000.0/256;
    for(int b=0;b<256;b++){double d=bins[b]-ex;chi2+=d*d/ex;}
    printf("  chi2=%.1f (pass<300)\n",chi2);
    T("chi2",chi2<300);
    printf("\n");

    /* ══════ 4. SPARSE-KEY CHI2 — ALL SIZES ══════ */
    printf("─── 4. Sparse-key chi2 (94%% zeros, fixed seed) ───\n");
    uint64_t seed=0x123456789ABCDEF0ULL;
    int sizes[]={16,32,48,64,128,256,512,1024,-1};
    for(int si=0;sizes[si]>=0;si++){
        double c=sparse_chi2(sizes[si],seed,50000);
        const char*mark=(c<68000)?"EXCELLENT":(c<90000)?"GOOD":(c<120000)?"OK":"POOR";
        printf("  %4dB: chi2=%.1f  %s\n",sizes[si],c,mark);
        if(c<120000)pass++;else fail++;
    }
    printf("\n");

    /* ══════ 5. SEED SWEEP — SPARSE 32B/64B ══════ */
    printf("─── 5. Seed sweep (sparse 32B + 64B, 200 seeds) ───\n");
    double worst32=0,best32=1e9,sum32=0;
    double worst64=0,best64=1e9,sum64=0;
    for(int si=0;si<200;si++){
        uint64_t s=(uint64_t)si*0x9E3779B97F4A7C15ULL;
        double c32=sparse_chi2(32,s,30000);
        double c64=sparse_chi2(64,s,30000);
        sum32+=c32;sum64+=c64;
        if(c32>worst32)worst32=c32;if(c32<best32)best32=c32;
        if(c64>worst64)worst64=c64;if(c64<best64)best64=c64;
    }
    printf("  32B: mean=%.1f  best=%.1f  worst=%.1f  spread=%.1f%%\n",
           sum32/200,best32,worst32,(worst32-best32)/best32*100);
    printf("  64B: mean=%.1f  best=%.1f  worst=%.1f  spread=%.1f%%\n",
           sum64/200,best64,worst64,(worst64-best64)/best64*100);
    int ok32=(worst32<120000),ok64=(worst64<120000);
    printf("  %s\n\n",(ok32&&ok64)?"PASS":"WARN");

    /* ══════ 6. MULTICOLLISION ══════ */
    printf("─── 6. Multicollision (100K unique keys) ───\n");
    #define MC (1<<18)
    uint64_t *tab=(uint64_t*)calloc(MC,sizeof(uint64_t));
    uint8_t *pres=(uint8_t*)calloc(MC,1);
    int coll=0;
    for(int i=0;i<100000;i++){
        uint8_t k[8];memcpy(k,&i,4);
        for(int j=4;j<8;j++)k[j]=(uint8_t)((i+j*7)&0xFF);
        h=plumshash(k,8,0xDEADBEEFCAFE1234ULL);
        uint64_t s=h&(MC-1);
        while(pres[s]&&tab[s]!=h)s=(s+1)&(MC-1);
        if(pres[s])coll++;else{tab[s]=h;pres[s]=1;}
    }
    printf("  Keys: 100000, collisions: %d, expected: ~0\n",coll);
    T("multicollision",coll==0);
    free(tab);free(pres);
    printf("\n");

    /* ══════ 7. KEY WHITENING BYPASS ══════ */
    printf("─── 7. Key whitening bypass ───\n");
    int cross_seed_coll=0;
    for(int i=0;i<50000;i+=2){
        uint8_t k1[8],k2[8];
        for(int j=0;j<8;j++){k1[j]=(uint8_t)((i+j)&0xFF);k2[j]=(uint8_t)((i+1+j)&0xFF);}
        if(plumshash(k1,8,0)==plumshash(k2,8,0)){
            /* Check 5 other seeds */
            int all=1;
            uint64_t seeds[]={1,42,0xDEAD,0xCAFE,0xFFFFFFFFFFFFFFFFULL};
            for(int s=0;s<5;s++)
                if(plumshash(k1,8,seeds[s])!=plumshash(k2,8,seeds[s])){all=0;break;}
            if(all)cross_seed_coll++;
        }
    }
    printf("  Cross-seed persistent collisions: %d / 25000\n",cross_seed_coll);
    T("whitening_bypass",cross_seed_coll==0);
    printf("\n");

    /* ══════ 8. DIFFERENTIAL ══════ */
    printf("─── 8. Differential analysis ───\n");
    int dist[65]={0};
    memset(buf,0xA5,64);
    for(int i=0;i<20000;i++){
        buf[0]=(uint8_t)(i&0xFF);buf[1]=(uint8_t)((i>>8)&0xFF);
        h=plumshash(buf,64,0xFEEDFACEC0FFEEEEULL);
        buf[31]^=0x01;
        uint64_t h2=plumshash(buf,64,0xFEEDFACEC0FFEEEEULL);
        buf[31]^=0x01;
        dist[pop(h^h2)]++;
    }
    double mean=0,var=0;
    for(int d=0;d<=64;d++)mean+=d*dist[d];mean/=20000;
    for(int d=0;d<=64;d++)var+=(d-mean)*(d-mean)*dist[d];var/=20000;
    printf("  mean=%.4f (ideal 32.0), std=%.4f (ideal 4.0)\n",mean,sqrt(var));
    T("differential",fabs(mean-32.0)<0.2&&fabs(sqrt(var)-4.0)<0.2);
    printf("\n");

    /* ══════ 9. LENGTH EXTENSION ══════ */
    printf("─── 9. Length extension ───\n");
    char *msg="Hello, World!";
    size_t len=strlen(msg);
    int same=0;
    for(int i=0;i<100000;i++){
        uint8_t ext[64];memcpy(ext,msg,len);
        ext[len]=(uint8_t)(i&0xFF);
        h=plumshash(msg,len,0xCAFEBABEDEADBEEFULL);
        uint64_t h2=plumshash(ext,len+1,0xCAFEBABEDEADBEEFULL);
        if((h&1)==(h2&1))same++;
    }
    double corr=(double)same/100000;
    printf("  bit-0 correlation: %.4f (ideal 0.500)\n",corr);
    T("length_extension",fabs(corr-0.5)<0.05);
    printf("\n");

    /* ══════ 10. SEED RECOVERY ══════ */
    printf("─── 10. Seed recovery resistance ───\n");
    int bit_counts[64]={0};
    for(int i=0;i<100000;i++){
        uint64_t s=0x0123456789ABCDEFULL+(uint64_t)i*0x9E3779B97F4A7C15ULL;
        uint8_t k[16];memset(k,(uint8_t)(i&0xFF),16);
        h=plumshash(k,16,s);
        for(int b=0;b<64;b++)if(h&(1ULL<<b))bit_counts[b]++;
    }
    double worst_bias=0;
    for(int b=0;b<64;b++){double bias=fabs((double)bit_counts[b]/100000-0.5);if(bias>worst_bias)worst_bias=bias;}
    printf("  worst bit bias: %.4f (%.1f%%)\n",worst_bias,worst_bias*100);
    T("seed_recovery",worst_bias<0.02);
    printf("\n");

    /* ══════ 11. SPEED ══════ */
    printf("─── 11. Speed ───\n");
    const int N=2000000;
    uint8_t *kb=(uint8_t*)malloc(4096);
    memset(kb,0xAB,4096);
    struct{int len;const char*l;}sz[]={{4,"4B"},{16,"16B"},{32,"32B"},{64,"64B"},{128,"128B"},{256,"256B"},{512,"512B"},{1024,"1KB"},{4096,"4KB"},{0,NULL}};
    uint64_t acc=0;
    for(int si=0;sz[si].l;si++){
        int iters=(sz[si].len<64)?N*4:N;
        acc+=plumshash(kb,sz[si].len,0);
        struct timespec t0,t1;
        clock_gettime(CLOCK_MONOTONIC,&t0);
        for(int i=0;i<iters;i++)acc+=plumshash(kb,sz[si].len,acc^(uint64_t)i);
        clock_gettime(CLOCK_MONOTONIC,&t1);
        double sec=(t1.tv_sec-t0.tv_sec)+(t1.tv_nsec-t0.tv_nsec)*1e-9;
        printf("  %-4s: %7.2f GB/s  %6.0f ns/h\n",sz[si].l,((double)sz[si].len*iters/1e9)/sec,sec*1e9/iters);
    }
    free(kb);
    printf("\n");

    /* ══════ RESULTS ══════ */
    printf("══════════════════════════════════════════════\n");
    printf("RESULTS: %d passed, %d warnings, %d failed\n",pass,warn,fail);
    printf("══════════════════════════════════════════════\n");
    return fail?1:0;
}
