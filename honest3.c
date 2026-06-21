#include <time.h>
#include <stdint.h>
#include <string.h>
#define VORTEXHASH_IMPLEMENTATION
/* Override finalizer shifts */
#undef VM1
#undef VM2
#define VM1 0x85EBCA77C2B2AE3DULL
#define VM2 0xBF58476D1CE4E5B9ULL
/* Patch finalizer inline */
static inline uint64_t vr(uint64_t x,int k){return(x<<k)|(x>>(64-k));}
#define VP 0x9E3779B97F4A7C15ULL
#define VM3 0x94D049BB133111EBULL

/* Fast path with {41,19,43,53} finalizer */
static uint64_t v_fast(const uint8_t*p,size_t l,uint64_t s){
    const uint8_t*e=p+l;uint64_t ba=s^(l*VP),h1=ba*VP,h2=ba*VM1,h3=ba*VM2,h4=ba*VM1;
    while(p+32<=e){h1^=*(const uint64_t*)p;p+=8;h1=vr(h1+h2,11);h2^=*(const uint64_t*)p;p+=8;h2=vr(h2+h3,17);h3^=*(const uint64_t*)p;p+=8;h3=vr(h3+h4,23);h4^=*(const uint64_t*)p;p+=8;h4=vr(h4+h1,57);}
    while(p+8<=e){h1^=*(const uint64_t*)p;p+=8;h1=vr(h1+h2,11);}
    uint64_t t=0;switch(e-p){case 7:t^=(uint64_t)p[6]<<48;case 6:t^=(uint64_t)p[5]<<40;case 5:t^=(uint64_t)p[4]<<32;case 4:t^=(uint64_t)p[3]<<24;case 3:t^=(uint64_t)p[2]<<16;case 2:t^=(uint64_t)p[1]<<8;case 1:t^=p[0];h1^=t;h1=vr(h1+h2,11);}
    h1^=h2;h3^=h4;h1^=h3;
    h1^=h1>>41;h1*=VM1;h1^=h1>>19;h1*=VM2;h1^=h1>>43;h1*=VP;h1^=h1>>53;
    return h1;
}
static uint64_t v_safe(const uint8_t*p,size_t l,uint64_t s){
    const uint8_t*e=p+l;uint64_t ba=s^(l*VP),h1=ba*VP,h2=ba*VM1,h3=ba*VM2,h4=ba*VM1,acc=ba^VM2;
    while(p+32<=e){uint64_t v1=*(const uint64_t*)p;p+=8;uint64_t v2=*(const uint64_t*)p;p+=8;uint64_t v3=*(const uint64_t*)p;p+=8;uint64_t v4=*(const uint64_t*)p;p+=8;
        h1^=v1;h1=vr(h1+h2,11);h2^=v2;h2=vr(h2+h3,17);h3^=v3;h3=vr(h3+h4,23);h4^=v4;h4=vr(h4+h1,57);acc^=v1^v2^v3^v4;acc=vr(acc,31);acc*=VP;}
    while(p+8<=e){uint64_t v=*(const uint64_t*)p;p+=8;h1^=v;h1=vr(h1+h2,11);acc^=v;acc=vr(acc,31);acc*=VP;}
    uint64_t t=0;switch(e-p){case 7:t^=(uint64_t)p[6]<<48;case 6:t^=(uint64_t)p[5]<<40;case 5:t^=(uint64_t)p[4]<<32;case 4:t^=(uint64_t)p[3]<<24;case 3:t^=(uint64_t)p[2]<<16;case 2:t^=(uint64_t)p[1]<<8;case 1:t^=p[0];}
    if(t){h1^=t;h1=vr(h1+h2,11);acc^=t;acc=vr(acc,31);acc*=VP;}
    h1^=h2;h3^=h4;h1^=h3;h1^=acc;
    h1^=h1>>41;h1*=VM1;h1^=h1>>19;h1*=VM2;h1^=h1>>43;h1*=VP;h1^=h1>>53;
    return h1;
}
uint64_t v3hash(const void*b,size_t l,uint64_t s){return l>=256?v_fast((const uint8_t*)b,l,s):v_safe((const uint8_t*)b,l,s);}

/* wyhash for comparison */
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

#include <stdio.h>
#include <stdlib.h>
#include <math.h>
static inline int pop(uint64_t x){x-=x>>1&0x5555555555555555ULL;x=(x&0x3333333333333333ULL)+(x>>2&0x3333333333333333ULL);x=(x+(x>>4))&0x0F0F0F0F0F0F0F0FULL;return(x*0x0101010101010101ULL)>>56;}
typedef uint64_t(*hf)(const void*,size_t,uint64_t);

int main(void){
    printf("=== V3 {41,19,43,53} vs wyhash64 ===\n\n");
    struct{const char*n;hf f;}hs[]={{"VortexV3",v3hash},{"wyhash64",wyhash64}};int nh=2;
    uint8_t wb[1024];volatile uint64_t ws=0;for(int i=0;i<2000;i++){ws^=v3hash(wb,256,i);ws^=wyhash64(wb,256,i);}

    /* Speed */
    printf("Speed (GB/s): %8s","");size_t szs[]={64,256,1024,65536};for(int si=0;si<4;si++)printf(" %8zu",szs[si]);printf("\n");
    for(int hi=0;hi<nh;hi++){printf("%-12s",hs[hi].n);
        for(int si=0;si<4;si++){size_t sz=szs[si];uint8_t*b=malloc(sz);for(size_t i=0;i<sz;i++)b[i]=i*0x9D+0x37;
            int it=sz<1024?30000000/sz:300000;volatile uint64_t s=0;clock_t st=clock();for(int i=0;i<it;i++)s^=hs[hi].f(b,sz,i);clock_t en=clock();
            printf(" %8.1f",sz*(double)it/((double)(en-st)/CLOCKS_PER_SEC)/1e9);free(b);}printf(" GB/s\n");}

    /* Avalanche */
    printf("\nAvalanche:\n");
    for(int hi=0;hi<nh;hi++){uint8_t buf[256];for(int i=0;i<256;i++)buf[i]=i*0x9D+0x37;uint64_t base=hs[hi].f(buf,256,0);double lo=100;
        for(int by=0;by<32;by++)for(int bi=0;bi<8;bi++){buf[by]^=(1u<<bi);uint64_t hv=hs[hi].f(buf,256,0);buf[by]^=(1u<<bi);double p=pop(base^hv)/64.0*100;if(p<lo)lo=p;}
        printf("%-12s worst=%.1f%%\n",hs[hi].n,lo);}

    /* Chi2 */
    printf("\nChi2:\n");
    for(int hi=0;hi<nh;hi++){int bins[256]={0};for(int i=0;i<256000;i++)bins[hs[hi].f(&i,4,i)&255]++;
        double ex=1000.0,chi2=0;for(int b=0;b<256;b++){double d=bins[b]-ex;chi2+=d*d/ex;}
        printf("%-12s chi2=%.1f\n",hs[hi].n,chi2);}

    /* Sparse */
    printf("\nSparse:\n");
    for(int hi=0;hi<nh;hi++){int cols=0;uint64_t seen[20000]={0};int ngen=0;uint8_t key[256];
        for(int pos=0;pos<128&&ngen<20000;pos++)for(int val=1;val<256&&ngen<20000;val++){int kl=8+(pos%57);memset(key,0,kl);key[kl-1]=val;if(pos&1)key[0]=pos^val;seen[ngen++]=hs[hi].f(key,kl,0);}
        for(int i=0;i<ngen;i++)for(int j=i+1;j<ngen;j++)if(seen[i]==seen[j])cols++;
        printf("%-12s %d collisions\n",hs[hi].n,cols);}
    return 0;
}
