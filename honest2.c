#define VORTEXHASH_IMPLEMENTATION
#include "vortexhash.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#include <math.h>

/* ===== WYHASH64 v4.3 canonical ===== */
static inline void wymum(uint64_t*A,uint64_t*B){
#ifdef __SIZEOF_INT128__
    __uint128_t r=*A;r*=*B;*A=(uint64_t)r;*B=(uint64_t)(r>>64);
#else
    uint64_t ha=*A>>32,hb=*B>>32,la=(uint32_t)*A,lb=(uint32_t)*B;
    uint64_t rh=ha*hb,rm0=ha*lb,rm1=hb*la,rl=la*lb,t=rl+(rm0<<32),c=t<rl;
    uint64_t lo=t+(rm1<<32);c+=lo<t;uint64_t hi=rh+(rm0>>32)+(rm1>>32)+c;*A=lo;*B=hi;
#endif
}
static inline uint64_t wymix(uint64_t A,uint64_t B){wymum(&A,&B);return A^B;}
static inline uint64_t wyr8(const uint8_t*p){uint64_t v;memcpy(&v,p,8);return v;}
static inline uint64_t wyr4(const uint8_t*p){uint32_t v;memcpy(&v,p,4);return v;}
static inline uint64_t wyr3(const uint8_t*p,size_t k){return(((uint64_t)p[0])<<16)|(((uint64_t)p[k>>1])<<8)|p[k-1];}
static const uint64_t wyp[4]={0x2d358dccaa6c78a5ULL,0x8bb84b93962eacc9ULL,0x4b33a62ed433d4a3ULL,0x4d5a2da51de1aa47ULL};

uint64_t wyhash64(const void*key,size_t len,uint64_t seed){
    const uint8_t*p=(const uint8_t*)key;seed^=wymix(seed^wyp[0],wyp[1]);uint64_t a,b;
    if(len<=16){if(len>=4){a=(wyr4(p)<<32)|wyr4(p+((len>>3)<<2));b=(wyr4(p+len-4)<<32)|wyr4(p+len-4-((len>>3)<<2));}
    else if(len>0){a=wyr3(p,len);b=0;}else a=b=0;}
    else{size_t i=len;if(i>=48){uint64_t s1=seed,s2=seed;do{seed=wymix(wyr8(p)^wyp[1],wyr8(p+8)^seed);
    s1=wymix(wyr8(p+16)^wyp[2],wyr8(p+24)^s1);s2=wymix(wyr8(p+32)^wyp[3],wyr8(p+40)^s2);p+=48;i-=48;}while(i>=48);seed^=s1^s2;}
    while(i>16){seed=wymix(wyr8(p)^wyp[1],wyr8(p+8)^seed);i-=16;p+=16;}a=wyr8(p+i-16);b=wyr8(p+i-8);}
    a^=wyp[1];b^=seed;wymum(&a,&b);return wymix(a^wyp[0]^len,b^wyp[1]);
}

static inline int pop(uint64_t x){x-=x>>1&0x5555555555555555ULL;x=(x&0x3333333333333333ULL)+(x>>2&0x3333333333333333ULL);x=(x+(x>>4))&0x0F0F0F0F0F0F0F0FULL;return(x*0x0101010101010101ULL)>>56;}
typedef uint64_t(*hf)(const void*,size_t,uint64_t);

int main(void){
    printf("═══════════════════════════════════════\n");
    printf("  VortexHash v3 (mul-lane-mix) vs wyhash64\n");
    printf("═══════════════════════════════════════\n\n");

    struct{const char*n;hf f;}hs[]={{"VortexHash",vortexhash},{"wyhash64",wyhash64}};
    int nh=2;

    /* Warmup */
    uint8_t wb[1024];volatile uint64_t ws=0;
    for(int i=0;i<2000;i++){ws^=vortexhash(wb,i&255?256:1024,i);ws^=wyhash64(wb,i&255?256:1024,i);}

    /* Speed */
    printf("─── SPEED (GB/s) ───\n%-12s","");size_t szs[]={8,32,64,256,1024,65536,1048576};int ns=7;
    for(int si=0;si<ns;si++)printf(" %8zu",szs[si]);printf("\n");
    for(int hi=0;hi<nh;hi++){printf("%-12s",hs[hi].n);
        for(int si=0;si<ns;si++){size_t sz=szs[si];uint8_t*b=malloc(sz);for(size_t i=0;i<sz;i++)b[i]=i*0x9D+0x37;
            int it=sz<1024?50000000/sz:sz<65536?500000:50000;if(it<10)it=10;
            volatile uint64_t s=0;clock_t st=clock();for(int i=0;i<it;i++)s^=hs[hi].f(b,sz,i);clock_t en=clock();
            printf(" %8.1f",sz*(double)it/((double)(en-st)/CLOCKS_PER_SEC)/1e9);free(b);}printf(" GB/s\n");}

    /* Quality */
    printf("\n─── AVALANCHE ───\n");
    for(int hi=0;hi<nh;hi++){uint8_t buf[256];for(int i=0;i<256;i++)buf[i]=i*0x9D+0x37;
        uint64_t base=hs[hi].f(buf,256,0);double lo=100,sum=0;
        for(int by=0;by<32;by++)for(int bi=0;bi<8;bi++){buf[by]^=(1u<<bi);uint64_t hv=hs[hi].f(buf,256,0);buf[by]^=(1u<<bi);double p=pop(base^hv)/64.0*100;sum+=p;if(p<lo)lo=p;}
        printf("%-12s avg=%.1f%% worst=%.1f%%\n",hs[hi].n,sum/256,lo);}

    printf("\n─── CHI2 ───\n");
    for(int hi=0;hi<nh;hi++){int bins[256]={0};
        for(int i=0;i<256000;i++)bins[hs[hi].f(&i,4,i)&255]++;
        double ex=1000.0,chi2=0;for(int b=0;b<256;b++){double d=bins[b]-ex;chi2+=d*d/ex;}
        printf("%-12s chi2=%.1f\n",hs[hi].n,chi2);}

    printf("\n─── SPARSE ───\n");
    for(int hi=0;hi<nh;hi++){int cols=0;uint64_t seen[20000]={0};int ngen=0;uint8_t key[256];
        for(int pos=0;pos<128&&ngen<20000;pos++)for(int val=1;val<256&&ngen<20000;val++){int kl=8+(pos%57);memset(key,0,kl);key[kl-1]=val;if(pos&1)key[0]=pos^val;seen[ngen++]=hs[hi].f(key,kl,0);}
        for(int i=0;i<ngen;i++)for(int j=i+1;j<ngen;j++)if(seen[i]==seen[j])cols++;
        printf("%-12s %d collisions\n",hs[hi].n,cols);}

    printf("\n─── 64-bit COLLISIONS ───\n");
    for(int hi=0;hi<nh;hi++){uint64_t*ha=malloc(50000*8);
        for(int i=0;i<50000;i++)ha[i]=hs[hi].f(&i,4,0);
        uint32_t*s32=calloc(1<<20,4);int d=0;
        for(int i=0;i<50000;i++){uint32_t sl=ha[i]>>32&((1u<<20)-1),tg=ha[i];
            if(s32[sl]==0)s32[sl]=tg;else if(s32[sl]==tg)for(int j=0;j<i;j++)if(ha[j]==ha[i]){d++;break;}}
        printf("%-12s %d collisions\n",hs[hi].n,d);free(ha);free(s32);}

    printf("\n═══════════════════════════════════════\n");
    return 0;
}
