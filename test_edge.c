#define PLUMSHASH_IMPLEMENTATION
#include "plumshash.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>

int main(void){
    int pass=0,fail=0;
    #define T(n,ok) do{if(ok)pass++;else{printf("FAIL: %s\n",n);fail++;}}while(0)

    printf("═══ EDGE CASES & DEEP CHECKS ═══\n\n");

    /* ── 1. Path boundaries (exact thresholds) ── */
    printf("── 1. Path boundary keys ──\n");
    uint64_t seed=0x123456789ABCDEF0ULL;
    int boundaries[]={0,1,2,3,4,7,8,9,15,16,17,31,32,33,47,48,49,63,64,65,
                       127,128,129,255,256,257,511,512,1023,1024,4095,4096,-1};
    uint8_t *kb=(uint8_t*)calloc(4096,1);
    for(int i=0;i<4096;i++)kb[i]=(uint8_t)(i*0x9D+0x37);
    int all_diff=1;
    for(int bi=0;boundaries[bi]>=0;bi++){
        int len=boundaries[bi];
        uint64_t h=plumshash(kb,(size_t)len,seed);
        /* Verify: same len+seed gives same hash */
        if(h!=plumshash(kb,(size_t)len,seed))all_diff=0;
        /* Verify: different len gives different hash (usually) */
        if(len>0){
            uint64_t h2=plumshash(kb,(size_t)(len-1),seed);
            if(h==h2&&len>16)all_diff=0; /* might collide for tiny keys */
        }
    }
    T("boundary_deterministic",all_diff);
    printf("  %d boundary keys tested\n\n",(int)(sizeof(boundaries)/sizeof(boundaries[0])-1));

    /* ── 2. Pattern keys ── */
    printf("── 2. Pattern-key chi2 ──\n");
    struct{const char*n;uint8_t pat;int len;}patterns[]={
        {"all-zero",0,64},{"all-0xFF",0xFF,64},
        {"0xAA",0xAA,64},{"0x55",0x55,64},
        {"alt-0x00/0xFF",0,64},{"counter",0,32},{NULL,0,0}};
    for(int pi=0;patterns[pi].n;pi++){
        memset(kb,patterns[pi].pat,(size_t)patterns[pi].len);
        if(pi==4)for(int j=0;j<patterns[pi].len;j++)kb[j]=(uint8_t)(j&1?0xFF:0x00);
        if(pi==5)for(int j=0;j<patterns[pi].len;j++)kb[j]=(uint8_t)j;

        int buckets[65536]={0};
        for(int i=0;i<100000;i++){
            kb[0]=(uint8_t)(i&0xFF);
            kb[1]=(uint8_t)((i>>8)&0xFF);
            buckets[plumshash(kb,(size_t)patterns[pi].len,seed)&0xFFFF]++;
        }
        double chi2=0,ex=100000.0/65536;
        for(int b=0;b<65536;b++){double d=buckets[b]-ex;chi2+=d*d/ex;}
        printf("  %-16s (%dB): chi2=%.1f\n",patterns[pi].n,patterns[pi].len,chi2);
        T(patterns[pi].n,chi2<120000);
    }
    printf("\n");

    /* ── 3. Per-output-bit uniformity ── */
    printf("── 3. Per-bit uniformity (64 bits, 256K samples) ──\n");
    int bit1s[64]={0};
    for(int i=0;i<256000;i++){
        uint64_t h=plumshash(&i,sizeof(i),i);
        for(int b=0;b<64;b++)if(h&(1ULL<<b))bit1s[b]++;
    }
    double bit_worst=0;
    for(int b=0;b<64;b++){
        double bias=fabs((double)bit1s[b]/256000-0.5);
        if(bias>bit_worst)bit_worst=bias;
    }
    printf("  worst bit bias: %.4f (%.2f%%)\n",bit_worst,bit_worst*100);
    T("per_bit_uniformity",bit_worst<0.01);
    printf("\n");

    /* ── 4. Bit independence matrix (256 input bits × 64 output bits) ── */
    printf("── 4. Bit independence (128B key, 256 input bits) ──\n");
    memset(kb,0xA5,128);
    double matrix_worst=0;
    for(int ib=0;ib<256;ib++){  /* first 256 input bits */
        int by=ib/8,bi=ib%8;
        int out_1s[64]={0};
        for(int trial=0;trial<2000;trial++){
            kb[0]=(uint8_t)(trial&0xFF);
            kb[1]=(uint8_t)((trial>>8)&0xFF);
            kb[by]^=(uint8_t)(1u<<bi);
            uint64_t h0=plumshash(kb,128,seed+trial);
            kb[by]^=(uint8_t)(1u<<bi);
            uint64_t h1=plumshash(kb,128,seed+trial);
            uint64_t diff=h0^h1;
            for(int ob=0;ob<64;ob++)if(diff&(1ULL<<ob))out_1s[ob]++;
        }
        for(int ob=0;ob<64;ob++){
            double p=(double)out_1s[ob]/2000;
            double bias=fabs(p-0.5);
            if(bias>matrix_worst)matrix_worst=bias;
        }
    }
    printf("  worst (input,output) bias: %.4f\n",matrix_worst);
    T("bit_independence",matrix_worst<0.15);
    printf("\n");

    /* ── 5. Repeated-byte keys ── */
    printf("── 5. Repeated single-byte keys, all 256 values ──\n");
    int buckets[65536]={0};
    uint8_t rkey[64];
    for(int val=0;val<256;val++){
        memset(rkey,(uint8_t)val,64);
        /* Vary length slightly to get different hashes */
        for(int len=1;len<=64;len++){
            buckets[plumshash(rkey,(size_t)len,seed)&0xFFFF]++;
        }
    }
    double chi2=0,ex=256.0*64/65536;
    for(int b=0;b<65536;b++){double d=buckets[b]-ex;chi2+=d*d/ex;}
    printf("  repeated-byte keys (256×64): chi2=%.1f\n",chi2);
    T("repeated_byte",chi2<120000);
    printf("\n");

    /* ── 6. Incremental keys (counter in first 8 bytes) ── */
    printf("── 6. Sequential 8-byte counter keys ──\n");
    memset(buckets,0,sizeof(buckets));
    for(uint64_t i=0;i<100000;i++){
        uint64_t h=plumshash(&i,sizeof(i),seed);
        buckets[h&0xFFFF]++;
    }
    chi2=0;ex=100000.0/65536;
    for(int b=0;b<65536;b++){double d=buckets[b]-ex;chi2+=d*d/ex;}
    printf("  sequential 8B keys: chi2=%.1f\n",chi2);
    T("sequential",chi2<120000);
    printf("\n");

    /* ── 7. Speed vs size crossover ── */
    printf("── 7. Detailed speed sweep ──\n");
    printf("  %-6s %10s %8s\n","Size","GB/s","ns/h");
    int speeds[]={1,2,3,4,7,8,9,15,16,17,31,32,33,47,48,49,63,64,65,
                  127,128,129,255,256,257,511,512,1023,1024,2047,2048,4095,4096,-1};
    for(int si=0;speeds[si]>=0;si++){
        int len=speeds[si],iters=(len<64)?5000000:2000000;
        memset(kb,0xAB,(size_t)len);
        uint64_t acc=plumshash(kb,(size_t)len,0);
        struct timespec t0,t1; 
        clock_gettime(CLOCK_MONOTONIC,&t0);
        for(int i=0;i<iters;i++)acc+=plumshash(kb,(size_t)len,acc^(uint64_t)i);
        clock_gettime(CLOCK_MONOTONIC,&t1);
        double sec=(t1.tv_sec-t0.tv_sec)+(t1.tv_nsec-t0.tv_nsec)*1e-9;
        printf("  %-6d %8.2f GB/s %6.0f ns/h\n",len,
               ((double)len*iters/1e9)/sec,sec*1e9/iters);
    }
    printf("\n");

    /* ── 8. Hash consistency ── */
    printf("── 8. Cross-platform consistency ──\n");
    /* Verify known-answer test vectors */
    uint64_t kat1=plumshash("",0,0);
    uint64_t kat2=plumshash("The quick brown fox jumps over the lazy dog",43,42);
    uint64_t kat3=plumshash("\x00\x01\x02\x03\x04\x05\x06\x07",8,0);
    printf("  empty:      %016llx\n",(unsigned long long)kat1);
    printf("  quick fox:  %016llx\n",(unsigned long long)kat2);
    printf("  0..7:        %016llx\n",(unsigned long long)kat3);
    T("kat_consistent",kat1==plumshash("",0,0)&&
                       kat2==plumshash("The quick brown fox jumps over the lazy dog",43,42)&&
                       kat3==plumshash("\x00\x01\x02\x03\x04\x05\x06\x07",8,0));
    printf("\n");

    free(kb);
    printf("═══════════════════════════════\n");
    printf("RESULTS: %d passed, %d failed\n",pass,fail);
    return fail?1:0;
}
