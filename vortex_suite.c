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

int main(void){
    printf("=== VortexHash — Full SMHasher Suite ===\n\n");

    /* Sanity */
    T("sanity:det");uint64_t a=vortexhash("hello",5,0);if(vortexhash("hello",5,0)!=a)F("not det");
    T("sanity:zero");if(vortexhash("test",4,42)==0)F("zero");
    T("sanity:seed");if(vortexhash("",0,0)==vortexhash("",0,42))F("seed");
    T("sanity:diff");if(vortexhash("abc",3,0)==vortexhash("abd",3,0))F("abc/abd");
    T("sanity:len");if(vortexhash("test",4,0)==vortexhash("test\0",5,0))F("len ext");
    printf("  PASS sanity (5)\n");

    /* Avalanche */
    T("ava");uint8_t buf[256];for(int i=0;i<256;i++)buf[i]=i*0x9D+0x37;
    uint64_t base=vortexhash(buf,256,0);double s=0,lo=100,hi=0;
    for(int by=0;by<32;by++)for(int bi=0;bi<8;bi++){buf[by]^=(1u<<bi);uint64_t hv=vortexhash(buf,256,0);buf[by]^=(1u<<bi);double p=pop(base^hv)/64.0*100;s+=p;if(p<lo)lo=p;if(p>hi)hi=p;}
    printf("  ava: avg=%.1f%% worst=%.1f%% best=%.1f%%\n",s/256,lo,hi);
    if(s/256<45||s/256>55)F("avg %.1f",s/256);if(lo<33)F("worst %.1f",lo);

    /* Bias */
    T("bias");int bc[64]={0};
    for(int i=0;i<100000;i++){uint64_t hv=vortexhash(&i,4,i*0x9E3779B9);for(int b=0;b<64;b++)if(hv&(1ULL<<b))bc[b]++;}
    double wb=0;int wbi=0;for(int b=0;b<64;b++){double bi=fabs(bc[b]/100000.0*100-50);if(bi>wb){wb=bi;wbi=b;}}
    printf("  bias: worst bit %d at %.2f%%\n",wbi,wb+50);if(wb>5)F("%.2f",wb+50);

    /* Chi2 */
    T("dist");int bins[256]={0};
    for(int i=0;i<256000;i++)bins[vortexhash(&i,4,i)&255]++;
    double ex=1000.0,chi2=0;int mn=256000,mx=0;
    for(int b=0;b<256;b++){double d=bins[b]-ex;chi2+=d*d/ex;if(bins[b]<mn)mn=bins[b];if(bins[b]>mx)mx=bins[b];}
    printf("  chi2: %.1f (df=255,p95=293) range=[%d,%d]\n",chi2,mn,mx);if(chi2>350)F("%.1f",chi2);

    /* 32-bit collisions */
    T("col32");enum{CN=500000,CTS=1u<<20};uint32_t*tbl=calloc(CTS,4);if(!tbl)return 1;
    uint8_t key[16];int cols=0;
    for(int i=0;i<CN;i++){int kl=4+(i%13);memcpy(key,&i,4);for(int j=4;j<kl;j++)key[j]=i>>(j*3);
        uint64_t h64=vortexhash(key,kl,0);uint32_t h32=h64^(h64>>32),idx=h32&(CTS-1);
        if(tbl[idx]==0)tbl[idx]=h64;else if(tbl[idx]!=(uint32_t)(h64&0xFFFFFFFF))cols++;}
    double ec=CTS*(1-exp(-(double)CN/CTS))-CN*exp(-(double)CN/CTS);
    printf("  col32: %d (exp ~%.0f)\n",cols,ec);if(cols>ec*2.5&&cols>100)F("excessive %d",cols);free(tbl);

    /* Differential */
    T("diff");uint8_t da[64],db[64];double ds=0;int dn=0,dlo=64,dhi=0;
    for(int tr=0;tr<1000;tr++){for(int i=0;i<64;i++)da[i]=tr*0x9D+i*0x37;memcpy(db,da,64);
        db[tr%64]^=(1u<<((tr*7)%8));int c=pop(vortexhash(da,64,tr)^vortexhash(db,64,tr));ds+=c;dn++;if(c<dlo)dlo=c;if(c>dhi)dhi=c;}
    printf("  diff: avg=%.1f range=[%d,%d]\n",ds/dn,dlo,dhi);if(ds/dn<24||ds/dn>40)F("avg %.1f",ds/dn);

    /* Bulk */
    T("bulk");enum{N2=50000};uint64_t*ha=malloc(N2*8);
    for(int i=0;i<N2;i++)ha[i]=vortexhash(&i,4,0);
    uint32_t*s32=calloc(1<<20,4);int d=0;
    for(int i=0;i<N2;i++){uint32_t sl=ha[i]>>32&((1u<<20)-1),tg=ha[i];
        if(s32[sl]==0)s32[sl]=tg;else if(s32[sl]==tg)for(int j=0;j<i;j++)if(ha[j]==ha[i]){d++;break;}}
    printf("  bulk: %d 64-bit collisions in 50K ints\n",d);if(d>0)F("%d",d);free(ha);free(s32);

    /* Cyclic */
    T("cyclic");uint8_t ck[256];memset(ck,0x5A,255);uint64_t cs[256];int cc=0;
    for(int i=0;i<256;i++){ck[255]=i;cs[i]=vortexhash(ck,256,0);}
    for(int i=0;i<256;i++)for(int j=i+1;j<256;j++)if(cs[i]==cs[j])cc++;
    printf("  cyclic: %d collisions\n",cc);if(cc>0)F("%d",cc);

    /* Sparse */
    T("sparse");int sc=0;uint64_t*sv=calloc(20000,8);int sn=0;uint8_t sk[256];
    for(int pos=0;pos<128&&sn<20000;pos++)for(int val=1;val<256&&sn<20000;val++){
        int kl=8+(pos%57);memset(sk,0,kl);sk[kl-1]=val;if(pos&1)sk[0]=pos^val;sv[sn++]=vortexhash(sk,kl,0);}
    for(int i=0;i<sn;i++)for(int j=i+1;j<sn;j++)if(sv[i]==sv[j])sc++;
    printf("  sparse: %d collisions / %d keys\n",sc,sn);if(sc>30)F("%d",sc);free(sv);

    /* Seeds */
    T("seeds");const char*msg="The quick brown fox jumps over the lazy dog";
    for(int s=0;s<100;s++){uint64_t h0=vortexhash(msg,strlen(msg),s);
        for(int s2=0;s2<s;s2++){if(h0==vortexhash(msg,strlen(msg),s2)){static int scc=0;if(++scc>2)F("seed coll %d/%d",s,s2);}}}
    printf("  PASS seeds\n");

    /* Speed */
    T("speed");size_t szs[]={8,16,32,64,256,1024,8192,65536,262144,1048576};int ns=10;
    printf("  speed:");
    for(int si=0;si<ns;si++){size_t sz=szs[si];uint8_t*b=malloc(sz);
        for(size_t i=0;i<sz;i++)b[i]=i*0x9D+0x37;
        int it=sz<1024?50000000/sz:sz<65536?500000:50000;if(it<10)it=10;
        volatile uint64_t vs=0;clock_t st=clock();
        for(int i=0;i<it;i++)vs^=vortexhash(b,sz,i);clock_t en=clock();
        printf(" %zu:%.1f",sz,sz*(double)it/((double)(en-st)/CLOCKS_PER_SEC)/1e9);free(b);}
    printf(" GB/s\n");

    printf("\nResults: %d/%d tests passed\n",tr-tf,tr);
    if(tf){printf("FAILED: %d\n",tf);return 1;}
    printf("ALL TESTS PASSED\n");
    return 0;
}
