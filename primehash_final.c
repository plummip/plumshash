/*
 * primehash_final.c — PrimeHash v2 with {11,19,35,47} rotations + 5r finalizer
 * Compares v1, v2, and wyhash64
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>
#include <time.h>

static inline uint64_t rd(const void *p){uint64_t v;memcpy(&v,p,8);return v;}
static inline uint64_t rot(uint64_t x,int k){return(x<<k)|(x>>(64-k));}
static inline int pop(uint64_t x){x-=x>>1&0x5555555555555555ULL;x=(x&0x3333333333333333ULL)+(x>>2&0x3333333333333333ULL);x=(x+(x>>4))&0x0F0F0F0F0F0F0F0FULL;return(x*0x0101010101010101ULL)>>56;}

#define PHI 0x9E3779B97F4A7C15ULL
#define M1  0x85EBCA77C2B2AE3DULL
#define M2  0xBF58476D1CE4E5B9ULL
#define M3  0x94D049BB133111EBULL
#define M4  0xA3C8B9D5E1F2A7B4ULL

/* v1: original {23,47,13,37} */
uint64_t v1(const void *b,size_t l,uint64_t s){
    const uint8_t *p=(const uint8_t*)b,*e=p+l;
    uint64_t ba=s^(l*PHI),h1=ba*PHI,h2=ba*M1,h3=ba*M2,h4=ba*M3;
    while(p+32<=e){
        h1^=rot(rd(p),23);h1*=PHI;p+=8;h2^=rot(rd(p),47);h2*=PHI;p+=8;
        h3^=rot(rd(p),13);h3*=PHI;p+=8;h4^=rot(rd(p),37);h4*=PHI;p+=8;
    }
    while(p+8<=e){h1^=rot(rd(p),23);h1*=PHI;p+=8;}
    uint64_t t=0;switch(e-p){
        case 7:t^=(uint64_t)p[6]<<48;case 6:t^=(uint64_t)p[5]<<40;case 5:t^=(uint64_t)p[4]<<32;
        case 4:t^=(uint64_t)p[3]<<24;case 3:t^=(uint64_t)p[2]<<16;case 2:t^=(uint64_t)p[1]<<8;
        case 1:t^=(uint64_t)p[0];h1^=rot(t,23);h1*=PHI;
    }
    h1^=h2;h3^=h4;h1^=h3;h1^=h1>>29;h1*=M1;h1^=h1>>31;h1*=M2;h1^=h1>>37;h1*=PHI;h1^=h1>>41;return h1;
}

/* v2: best rotations {11,19,35,47} + 5r finalizer */
uint64_t v2(const void *b,size_t l,uint64_t s){
    const uint8_t *p=(const uint8_t*)b,*e=p+l;
    uint64_t ba=s^(l*PHI),h1=ba*PHI,h2=ba*M1,h3=ba*M2,h4=ba*M3;
    while(p+32<=e){
        h1^=rot(rd(p),11);h1*=PHI;p+=8;h2^=rot(rd(p),19);h2*=PHI;p+=8;
        h3^=rot(rd(p),35);h3*=PHI;p+=8;h4^=rot(rd(p),47);h4*=PHI;p+=8;
    }
    while(p+8<=e){h1^=rot(rd(p),11);h1*=PHI;p+=8;}
    uint64_t t=0;switch(e-p){
        case 7:t^=(uint64_t)p[6]<<48;case 6:t^=(uint64_t)p[5]<<40;case 5:t^=(uint64_t)p[4]<<32;
        case 4:t^=(uint64_t)p[3]<<24;case 3:t^=(uint64_t)p[2]<<16;case 2:t^=(uint64_t)p[1]<<8;
        case 1:t^=(uint64_t)p[0];h1^=rot(t,11);h1*=PHI;
    }
    h1^=h2;h3^=h4;h1^=h3;
    h1^=h1>>17;h1*=M1;h1^=h1>>23;h1*=M2;h1^=h1>>29;h1*=M3;h1^=h1>>37;h1*=M4;h1^=h1>>43;
    return h1;
}

/* wyhash64 v4.3 canonical */
static inline void mum(uint64_t*A,uint64_t*B){
#ifdef __SIZEOF_INT128__
    __uint128_t r=*A;r*=*B;*A=(uint64_t)r;*B=(uint64_t)(r>>64);
#else
    uint64_t ha=*A>>32,hb=*B>>32,la=(uint32_t)*A,lb=(uint32_t)*B;
    uint64_t rh=ha*hb,rm0=ha*lb,rm1=hb*la,rl=la*lb,t=rl+(rm0<<32),c=t<rl;
    uint64_t lo=t+(rm1<<32);c+=lo<t;uint64_t hi=rh+(rm0>>32)+(rm1>>32)+c;*A=lo;*B=hi;
#endif
}
static inline uint64_t mix(uint64_t A,uint64_t B){mum(&A,&B);return A^B;}
static inline uint64_t r8(const uint8_t*p){uint64_t v;memcpy(&v,p,8);return v;}
static inline uint64_t r4(const uint8_t*p){uint32_t v;memcpy(&v,p,4);return v;}
static inline uint64_t r3(const uint8_t*p,size_t k){return(((uint64_t)p[0])<<16)|(((uint64_t)p[k>>1])<<8)|p[k-1];}
static const uint64_t wp[4]={0x2d358dccaa6c78a5ULL,0x8bb84b93962eacc9ULL,0x4b33a62ed433d4a3ULL,0x4d5a2da51de1aa47ULL};

uint64_t wy(const void*key,size_t len,uint64_t seed){
    const uint8_t*p=(const uint8_t*)key;seed^=mix(seed^wp[0],wp[1]);uint64_t a,b;
    if(len<=16){if(len>=4){a=(r4(p)<<32)|r4(p+((len>>3)<<2));b=(r4(p+len-4)<<32)|r4(p+len-4-((len>>3)<<2));}
    else if(len>0){a=r3(p,len);b=0;}else a=b=0;}
    else{size_t i=len;if(i>=48){uint64_t s1=seed,s2=seed;do{seed=mix(r8(p)^wp[1],r8(p+8)^seed);
    s1=mix(r8(p+16)^wp[2],r8(p+24)^s1);s2=mix(r8(p+32)^wp[3],r8(p+40)^s2);p+=48;i-=48;}while(i>=48);seed^=s1^s2;}
    while(i>16){seed=mix(r8(p)^wp[1],r8(p+8)^seed);i-=16;p+=16;}a=r8(p+i-16);b=r8(p+i-8);}
    a^=wp[1];b^=seed;mum(&a,&b);return mix(a^wp[0]^len,b^wp[1]);
}

/* ── harness ── */
typedef uint64_t(*hf)(const void*,size_t,uint64_t);
static int tr,tf;static const char*ct;
#define T(n) do{ct=n;tr++;}while(0)
#define F(f,...) do{fprintf(stderr,"  FAIL [%s] " f "\n",ct,##__VA_ARGS__);tf++;return;}while(0)
#define P() do{printf("  PASS %s\n",ct);}while(0)

static void ava(hf h,const char*t){
    T("ava");uint8_t b[256];for(int i=0;i<256;i++)b[i]=(uint8_t)(i*0x9D+0x37);
    uint64_t base=h(b,256,0);double s=0,lo=100,hi=0;int n=0;
    for(int by=0;by<32;by++)for(int bi=0;bi<8;bi++){
        b[by]^=(1u<<bi);uint64_t hv=h(b,256,0);b[by]^=(1u<<bi);
        double p=pop(base^hv)/64.0*100;s+=p;n++;if(p<lo)lo=p;if(p>hi)hi=p;
    }
    printf("  %-4s ava: avg=%.1f%% worst=%.1f%% best=%.1f%%\n",t,s/n,lo,hi);
    if(s/n<45||s/n>55)F("%s avg %.1f",t,s/n);if(lo<33)F("%s worst %.1f",t,lo);
}
static void bias(hf h,const char*t){
    T("bias");enum{BN=100000};int bc[64]={0};
    for(int i=0;i<BN;i++){uint64_t hv=h(&i,4,i*0x9E3779B9);for(int b=0;b<64;b++)if(hv&(1ULL<<b))bc[b]++;}
    double w=0;int wb=0;for(int b=0;b<64;b++){double bi=fabs(bc[b]/(double)BN*100-50);if(bi>w){w=bi;wb=b;}}
    printf("  %-4s bias: worst bit %2d at %.2f%%\n",t,wb,w+50);if(w>5)F("%s bias %.2f",t,w+50);
}
static void dist(hf h,const char*t){
    T("dist");enum{DB=256,DN=DB*1000};int bins[DB]={0};
    for(int i=0;i<DN;i++)bins[h(&i,4,i)&(DB-1)]++;
    double ex=DN/(double)DB,chi2=0;for(int b=0;b<DB;b++){double d=bins[b]-ex;chi2+=d*d/ex;}
    printf("  %-4s chi2: %.1f\n",t,chi2);if(chi2>350)F("%s chi2 %.1f",t,chi2);
}
static void col32(hf h,const char*t){
    T("col32");enum{CN=500000,CTS=1u<<20};uint32_t*tbl=calloc(CTS,4);if(!tbl){printf("  SKIP\n");return;}
    uint8_t key[16];int cols=0;
    for(int i=0;i<CN;i++){int kl=4+(i%13);memcpy(key,&i,4);for(int j=4;j<kl;j++)key[j]=(uint8_t)(i>>(j*3));
        uint64_t h64=h(key,kl,0);uint32_t h32=(uint32_t)(h64^(h64>>32)),idx=h32&(CTS-1);
        if(tbl[idx]==0)tbl[idx]=(uint32_t)(h64&0xFFFFFFFF);else if(tbl[idx]!=(uint32_t)(h64&0xFFFFFFFF))cols++;}
    double ec=CTS*(1-exp(-(double)CN/CTS))-CN*exp(-(double)CN/CTS);
    printf("  %-4s col32: %d (exp ~%.0f)\n",t,cols,ec);if(cols>ec*2.5&&cols>100)F("%s cols %d",t,cols);free(tbl);
}
static void diff(hf h,const char*t){
    T("diff");uint8_t a[64],b[64];double s=0;int n=0,lo=64,hi=0;
    for(int tr=0;tr<1000;tr++){for(int i=0;i<64;i++)a[i]=(uint8_t)(tr*0x9D+i*0x37);memcpy(b,a,64);
        b[tr%64]^=(1u<<((tr*7)%8));int c=pop(h(a,64,tr)^h(b,64,tr));s+=c;n++;if(c<lo)lo=c;if(c>hi)hi=c;}
    printf("  %-4s diff: avg=%.1f range=[%d,%d]\n",t,s/n,lo,hi);if(s/n<24||s/n>40)F("%s diff %.1f",t,s/n);
}
static void spd(hf h,const char*t){
    T("spd");int sz[]={1,4,8,16,32,64,256,1024},ns=8;printf("  %-4s spd:",t);double s=0;
    for(int i=0;i<ns;i++){uint8_t*buf=malloc(sz[i]);if(!buf)continue;for(int j=0;j<sz[i];j++)buf[j]=(uint8_t)(j*0x9D+0x37);
        int it=sz[i]<64?20000000:sz[i]<256?5000000:1000000;volatile uint64_t v=0;clock_t st=clock();
        for(int j=0;j<it;j++)v^=h(buf,sz[i],j);clock_t en=clock();
        double gb=(sz[i]*it/((double)(en-st)/CLOCKS_PER_SEC))/1e9;if(gb>0){printf(" %dB:%.1f",sz[i],gb);s+=gb;}free(buf);}
    printf(" | avg:%.1f GB/s\n",s/ns);
}

int main(void){
    printf("=== PrimeHash v1 vs v2 vs wyhash64 ===\n\n");
    tr=tf=0;printf("── v1 {23,47,13,37} ──\n");ava(v1,"v1");bias(v1,"v1");dist(v1,"v1");col32(v1,"v1");diff(v1,"v1");spd(v1,"v1");
    int r1=tr,f1=tf;
    tr=tf=0;printf("\n── v2 {11,19,35,47} ──\n");ava(v2,"v2");bias(v2,"v2");dist(v2,"v2");col32(v2,"v2");diff(v2,"v2");spd(v2,"v2");
    int r2=tr,f2=tf;
    tr=tf=0;printf("\n── wyhash64 ──\n");ava(wy,"wy");bias(wy,"wy");dist(wy,"wy");col32(wy,"wy");diff(wy,"wy");spd(wy,"wy");
    int r3=tr,f3=tf;
    printf("\n=== RESULTS ===\n");
    printf("v1: %d/%d  v2: %d/%d  wy: %d/%d\n",r1-f1,r1,r2-f2,r2,r3-f3,r3);
    return(f1||f2||f3)?1:0;
}
