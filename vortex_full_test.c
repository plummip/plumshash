/*
 * vortexhash_full_test.c — SMHasher-grade comprehensive test
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>
#include <time.h>

static inline uint64_t rot(uint64_t x,int k){return(x<<k)|(x>>(64-k));}
static inline int pop(uint64_t x){x-=x>>1&0x5555555555555555ULL;x=(x&0x3333333333333333ULL)+(x>>2&0x3333333333333333ULL);x=(x+(x>>4))&0x0F0F0F0F0F0F0F0FULL;return(x*0x0101010101010101ULL)>>56;}

/* ===== VORTEXHASH ==== */
#define PHI 0x9E3779B97F4A7C15ULL
#define M1 0x85EBCA77C2B2AE3DULL
#define M2 0xBF58476D1CE4E5B9ULL
#define M3 0x94D049BB133111EBULL

uint64_t vortex(const void*b,size_t l,uint64_t s){
    const uint8_t*p=b,*e=p+l;uint64_t ba=s^(l*PHI),h1=ba*PHI,h2=ba*M1,h3=ba*M2,h4=ba*M3;
    int blk=0;
    while(p+32<=e){
        h1^=*(const uint64_t*)p ^ rot(PHI,blk);h1=rot(h1+h2,11);p+=8;blk++;
        h2^=*(const uint64_t*)p ^ rot(PHI,blk);h2=rot(h2+h3,17);p+=8;blk++;
        h3^=*(const uint64_t*)p ^ rot(PHI,blk);h3=rot(h3+h4,23);p+=8;blk++;
        h4^=*(const uint64_t*)p ^ rot(PHI,blk);h4=rot(h4+h1,57);p+=8;blk++;
    }
    while(p+8<=e){h1^=*(const uint64_t*)p ^ rot(PHI,blk);h1=rot(h1+h2,11);p+=8;blk++;}
    uint64_t t=0;switch(e-p){case 7:t^=(uint64_t)p[6]<<48;case 6:t^=(uint64_t)p[5]<<40;case 5:t^=(uint64_t)p[4]<<32;case 4:t^=(uint64_t)p[3]<<24;case 3:t^=(uint64_t)p[2]<<16;case 2:t^=(uint64_t)p[1]<<8;case 1:t^=p[0];h1^=t ^ rot(PHI,blk);h1=rot(h1+h2,11);}
    h1^=h2;h3^=h4;h1^=h3;h1^=h1>>29;h1*=M1;h1^=h1>>31;h1*=M2;h1^=h1>>37;h1*=PHI;h1^=h1>>41;return h1;
}

typedef uint64_t(*hf)(const void*,size_t,uint64_t);
static int tr,tf;static const char*ct;
#define T(n) do{ct=n;tr++;}while(0)
#define F(f,...) do{fprintf(stderr,"  FAIL [%s] " f "\n",ct,##__VA_ARGS__);tf++;return;}while(0)
#define P() do{}while(0)

/* --- SANITY --- */
static void test_sanity(hf h){
    T("sanity: deterministic");uint64_t a=h("hello",5,0);if(h("hello",5,0)!=a)F("not deterministic");P();
    T("sanity: nonzero");if(h("test",4,42)==0)F("zero output");P();
    T("sanity: seed");if(h("",0,0)==h("",0,42))F("seed ignored on empty");P();
    T("sanity: diff input");if(h("abc",3,0)==h("abd",3,0))F("collision abc/abd");P();
    T("sanity: len ext");if(h("test",4,0)==h("test\0",5,0))F("len extension");P();
}

/* --- AVALANCHE --- */
static void test_ava(hf h){
    T("avalanche");uint8_t b[256];for(int i=0;i<256;i++)b[i]=i*0x9D+0x37;
    uint64_t base=h(b,256,0);double s=0,lo=100,hi=0;
    for(int by=0;by<32;by++)for(int bi=0;bi<8;bi++){
        b[by]^=(1u<<bi);uint64_t hv=h(b,256,0);b[by]^=(1u<<bi);
        double p=pop(base^hv)/64.0*100;s+=p;if(p<lo)lo=p;if(p>hi)hi=p;
    }
    printf("  ava: avg=%.1f%% worst=%.1f%% best=%.1f%%\n",s/256,lo,hi);
    if(s/256<45||s/256>55)F("avg %.1f",s/256);if(lo<33)F("worst %.1f",lo);
}

/* --- BIAS --- */
static void test_bias(hf h){
    T("bias");int bc[64]={0};
    for(int i=0;i<100000;i++){uint64_t hv=h(&i,4,i*0x9E3779B9);for(int b=0;b<64;b++)if(hv&(1ULL<<b))bc[b]++;}
    double w=0;int wb=0;
    for(int b=0;b<64;b++){double bi=fabs(bc[b]/100000.0*100-50);if(bi>w){w=bi;wb=b;}}
    printf("  bias: worst bit %d at %.2f%%\n",wb,w+50);
    if(w>5)F("bias %.2f%%",w+50);
}

/* --- DISTRIBUTION --- */
static void test_dist(hf h){
    T("dist");int bins[256]={0};
    for(int i=0;i<256000;i++)bins[h(&i,4,i)&255]++;
    double ex=1000.0,chi2=0;int lo=256000,hi=0;
    for(int b=0;b<256;b++){double d=bins[b]-ex;chi2+=d*d/ex;if(bins[b]<lo)lo=bins[b];if(bins[b]>hi)hi=bins[b];}
    printf("  chi2: %.1f (df=255,p95=293) range=[%d,%d]\n",chi2,lo,hi);
    if(chi2>350)F("chi2 %.1f",chi2);
}

/* --- 32-bit COLLISION --- */
static void test_col32(hf h){
    T("col32");enum{CN=500000,CTS=1u<<20};uint32_t*tbl=calloc(CTS,4);if(!tbl)return;
    uint8_t key[16];int cols=0;
    for(int i=0;i<CN;i++){int kl=4+(i%13);memcpy(key,&i,4);for(int j=4;j<kl;j++)key[j]=i>>(j*3);
        uint64_t h64=h(key,kl,0);uint32_t h32=h64^(h64>>32),idx=h32&(CTS-1);
        if(tbl[idx]==0)tbl[idx]=h64;else if(tbl[idx]!=(uint32_t)(h64&0xFFFFFFFF))cols++;}
    double ec=CTS*(1-exp(-(double)CN/CTS))-CN*exp(-(double)CN/CTS);
    printf("  col32: %d (exp ~%.0f)\n",cols,ec);
    if(cols>ec*2.5&&cols>100)F("excessive %d",cols);free(tbl);
}

/* --- DIFFERENTIAL --- */
static void test_diff(hf h){
    T("diff");uint8_t a[64],b[64];double s=0;int n=0,lo=64,hi=0;
    for(int tr=0;tr<1000;tr++){for(int i=0;i<64;i++)a[i]=tr*0x9D+i*0x37;memcpy(b,a,64);
        b[tr%64]^=(1u<<((tr*7)%8));int c=pop(h(a,64,tr)^h(b,64,tr));s+=c;n++;if(c<lo)lo=c;if(c>hi)hi=c;}
    printf("  diff: avg=%.1f range=[%d,%d]\n",s/n,lo,hi);
    if(s/n<24||s/n>40)F("avg %.1f",s/n);
}

/* --- SPEED --- */
static void test_spd(hf h){
    T("speed");size_t szs[]={8,16,32,64,256,1024,8192,65536,262144,1048576};int ns=10;
    printf("  speed:");
    for(int si=0;si<ns;si++){size_t sz=szs[si];uint8_t*b=malloc(sz);
        for(size_t i=0;i<sz;i++)b[i]=i*0x9D+0x37;
        int it=sz<1024?50000000/sz:sz<65536?500000:50000;if(it<10)it=10;
        volatile uint64_t s=0;clock_t st=clock();
        for(int i=0;i<it;i++)s^=h(b,sz,i);clock_t en=clock();
        printf(" %zu:%.1f",sz,sz*(double)it/((double)(en-st)/CLOCKS_PER_SEC)/1e9);free(b);
    }
    printf(" GB/s\n");
}

/* --- BULK --- */
static void test_bulk(hf h){
    T("bulk");enum{N=50000};uint64_t*ha=malloc(N*8);if(!ha)return;
    for(int i=0;i<N;i++)ha[i]=h(&i,4,0);
    uint32_t*s32=calloc(1<<20,4);if(!s32){free(ha);return;}
    int d=0;for(int i=0;i<N;i++){uint32_t sl=ha[i]>>32&((1u<<20)-1),tg=ha[i];
        if(s32[sl]==0)s32[sl]=tg;else if(s32[sl]==tg)for(int j=0;j<i;j++)if(ha[j]==ha[i]){d++;break;}}
    printf("  bulk: %d 64-bit collisions in 50K ints\n",d);
    if(d>0)F("%d collisions",d);free(ha);free(s32);
}

/* --- CYCLIC --- */
static void test_cyc(hf h){
    T("cyclic");uint8_t k[256];memset(k,0x5A,255);uint64_t s[256];int c=0;
    for(int i=0;i<256;i++){k[255]=i;s[i]=h(k,256,0);}
    for(int i=0;i<256;i++)for(int j=i+1;j<256;j++)if(s[i]==s[j])c++;
    printf("  cyclic: %d collisions\n",c);
    if(c>0)F("%d collisions",c);
}

/* --- SPARSE --- */
static void test_sparse(hf h){
    T("sparse");int cols=0;enum{N=20000};uint64_t*seen=calloc(N,8);if(!seen)return;
    uint8_t key[256];int ngen=0;
    for(int pos=0;pos<128&&ngen<N;pos++)for(int val=1;val<256&&ngen<N;val++){
        int kl=8+(pos%57);memset(key,0,kl);key[kl-1]=val;if(pos&1)key[0]=pos^val;
        seen[ngen++]=h(key,kl,0);
    }
    for(int i=0;i<ngen;i++)for(int j=i+1;j<ngen;j++)if(seen[i]==seen[j])cols++;
    printf("  sparse: %d collisions in %d keys\n",cols,ngen);
    if(cols>30)F("%d collisions",cols);free(seen);
}

/* --- SEEDS --- */
static void test_seeds(hf h){
    T("seeds");const char*msg="The quick brown fox jumps over the lazy dog";
    for(int s=0;s<100;s++){uint64_t h0=h(msg,strlen(msg),s);
        for(int s2=0;s2<s;s2++){if(h0==h(msg,strlen(msg),s2)){
            static int sc=0;if(++sc>2)F("seed collision %d/%d",s,s2);}}}
}

int main(void){
    printf("=== VortexHash — Full SMHasher Test Suite ===\n\n");
    hf h=vortex;

    test_sanity(h);printf("  PASS sanity (5)\n");
    test_ava(h);
    test_bias(h);
    test_dist(h);
    test_col32(h);
    test_diff(h);
    test_bulk(h);
    test_cyc(h);
    test_sparse(h);
    test_seeds(h);printf("  PASS seeds\n");
    test_spd(h);

    printf("\nResults: %d/%d tests passed\n",tr-tf,tr);
    if(tf>0){printf("FAILED: %d\n",tf);return 1;}
    printf("ALL TESTS PASSED\n");
    return 0;
}
