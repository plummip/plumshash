#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>
#include <time.h>
static inline uint64_t rd(const void*p){uint64_t v;memcpy(&v,p,8);return v;}
static inline uint64_t rot(uint64_t x,int k){return(x<<k)|(x>>(64-k));}
static inline int pop(uint64_t x){x-=x>>1&0x5555555555555555ULL;x=(x&0x3333333333333333ULL)+(x>>2&0x3333333333333333ULL);x=(x+(x>>4))&0x0F0F0F0F0F0F0F0FULL;return(x*0x0101010101010101ULL)>>56;}
#define PHI 0x9E3779B97F4A7C15ULL
#define M1 0x85EBCA77C2B2AE3DULL
#define M2 0xBF58476D1CE4E5B9ULL
#define M3 0x94D049BB133111EBULL
typedef uint64_t(*hf)(const void*,size_t,uint64_t);
uint64_t v1(const void*b,size_t l,uint64_t s){const uint8_t*p=b,*e=p+l;uint64_t ba=s^(l*PHI),h1=ba*PHI,h2=ba*M1,h3=ba*M2,h4=ba*M3;while(p+32<=e){h1^=rot(rd(p),23);h1*=PHI;p+=8;h2^=rot(rd(p),47);h2*=PHI;p+=8;h3^=rot(rd(p),13);h3*=PHI;p+=8;h4^=rot(rd(p),37);h4*=PHI;p+=8;}while(p+8<=e){h1^=rot(rd(p),23);h1*=PHI;p+=8;}uint64_t t=0;switch(e-p){case 7:t^=(uint64_t)p[6]<<48;case 6:t^=(uint64_t)p[5]<<40;case 5:t^=(uint64_t)p[4]<<32;case 4:t^=(uint64_t)p[3]<<24;case 3:t^=(uint64_t)p[2]<<16;case 2:t^=(uint64_t)p[1]<<8;case 1:t^=(uint64_t)p[0];h1^=rot(t,23);h1*=PHI;}h1^=h2;h3^=h4;h1^=h3;h1^=h1>>29;h1*=M1;h1^=h1>>31;h1*=M2;h1^=h1>>37;h1*=PHI;h1^=h1>>41;return h1;}
uint64_t v2(const void*b,size_t l,uint64_t s){const uint8_t*p=b,*e=p+l;uint64_t ba=s^(l*PHI),h1=ba*PHI,h2=ba*M1,h3=ba*M2,h4=ba*M3;while(p+32<=e){h1^=rot(rd(p),11);h1*=PHI;p+=8;h2^=rot(rd(p),19);h2*=PHI;p+=8;h3^=rot(rd(p),35);h3*=PHI;p+=8;h4^=rot(rd(p),47);h4*=PHI;p+=8;}while(p+8<=e){h1^=rot(rd(p),11);h1*=PHI;p+=8;}uint64_t t=0;switch(e-p){case 7:t^=(uint64_t)p[6]<<48;case 6:t^=(uint64_t)p[5]<<40;case 5:t^=(uint64_t)p[4]<<32;case 4:t^=(uint64_t)p[3]<<24;case 3:t^=(uint64_t)p[2]<<16;case 2:t^=(uint64_t)p[1]<<8;case 1:t^=(uint64_t)p[0];h1^=rot(t,11);h1*=PHI;}h1^=h2;h3^=h4;h1^=h3;h1^=h1>>29;h1*=M1;h1^=h1>>31;h1*=M2;h1^=h1>>37;h1*=PHI;h1^=h1>>41;return h1;}
static int tr,tf;const char*ct;
#define T(n) do{ct=n;tr++;}while(0)
#define F(f,...) do{fprintf(stderr,"  FAIL [%s] " f "\n",ct,##__VA_ARGS__);tf++;return;}while(0)
void ava(hf h,const char*t){T("ava");uint8_t b[256];for(int i=0;i<256;i++)b[i]=i*0x9D+0x37;uint64_t base=h(b,256,0);double s=0,lo=100,hi=0;for(int by=0;by<32;by++)for(int bi=0;bi<8;bi++){b[by]^=(1u<<bi);uint64_t hv=h(b,256,0);b[by]^=(1u<<bi);double p=pop(base^hv)/64.0*100;s+=p;if(p<lo)lo=p;if(p>hi)hi=p;}printf("  %-4s ava: avg=%.1f%% worst=%.1f%% best=%.1f%%\n",t,s/(32*8),lo,hi);if(s/(32*8)<45||s/(32*8)>55)F("%s avg %.1f",t,s/(32*8));if(lo<33)F("%s worst %.1f",t,lo);}
void bias(hf h,const char*t){T("bias");enum{N=100000};int bc[64]={0};for(int i=0;i<N;i++){uint64_t hv=h(&i,4,i*0x9E3779B9);for(int b=0;b<64;b++)if(hv&(1ULL<<b))bc[b]++;}double w=0;int wb=0;for(int b=0;b<64;b++){double bi=fabs(bc[b]/(double)N*100-50);if(bi>w){w=bi;wb=b;}}printf("  %-4s bias: worst bit %2d at %.2f%%\n",t,wb,w+50);if(w>5)F("%s bias %.2f",t,w+50);}
void dist(hf h,const char*t){T("dist");enum{DB=256,DN=DB*1000};int bins[DB]={0};for(int i=0;i<DN;i++)bins[h(&i,4,i)&(DB-1)]++;double ex=DN/(double)DB,chi2=0;for(int b=0;b<DB;b++){double d=bins[b]-ex;chi2+=d*d/ex;}printf("  %-4s chi2: %.1f\n",t,chi2);if(chi2>350)F("%s chi2 %.1f",t,chi2);}
void col32(hf h,const char*t){T("col32");enum{CN=500000,CTS=1u<<20};uint32_t*tbl=calloc(CTS,4);if(!tbl)return;uint8_t key[16];int cols=0;for(int i=0;i<CN;i++){int kl=4+(i%13);memcpy(key,&i,4);for(int j=4;j<kl;j++)key[j]=(uint8_t)(i>>(j*3));uint64_t h64=h(key,kl,0);uint32_t h32=(uint32_t)(h64^(h64>>32)),idx=h32&(CTS-1);if(tbl[idx]==0)tbl[idx]=(uint32_t)(h64&0xFFFFFFFF);else if(tbl[idx]!=(uint32_t)(h64&0xFFFFFFFF))cols++;}double ec=CTS*(1-exp(-(double)CN/CTS))-CN*exp(-(double)CN/CTS);printf("  %-4s col32: %d (exp ~%.0f)\n",t,cols,ec);if(cols>ec*2.5&&cols>100)F("%s cols %d",t,cols);free(tbl);}
void diff(hf h,const char*t){T("diff");uint8_t a[64],b[64];double s=0;int n=0,lo=64,hi=0;for(int tr=0;tr<1000;tr++){for(int i=0;i<64;i++)a[i]=tr*0x9D+i*0x37;memcpy(b,a,64);b[tr%64]^=(1u<<((tr*7)%8));int c=pop(h(a,64,tr)^h(b,64,tr));s+=c;n++;if(c<lo)lo=c;if(c>hi)hi=c;}printf("  %-4s diff: avg=%.1f range=[%d,%d]\n",t,s/n,lo,hi);if(s/n<24||s/n>40)F("%s diff %.1f",t,s/n);}
void spd(hf h,const char*t){T("spd");int sz[]={1,4,8,16,32,64,256,1024},ns=8;printf("  %-4s spd:",t);double s=0;for(int i=0;i<ns;i++){uint8_t*buf=malloc(sz[i]);if(!buf)continue;for(int j=0;j<sz[i];j++)buf[j]=j*0x9D+0x37;int it=sz[i]<64?20000000:sz[i]<256?5000000:1000000;volatile uint64_t v=0;clock_t st=clock();for(int j=0;j<it;j++)v^=h(buf,sz[i],j);clock_t en=clock();double gb=(sz[i]*it/((double)(en-st)/CLOCKS_PER_SEC))/1e9;if(gb>0){printf(" %dB:%.1f",sz[i],gb);s+=gb;}free(buf);}printf(" | avg:%.1f GB/s\n",s/ns);}
int main(void){printf("=== PrimeHash: v1 {23,47,13,37} vs v2 {11,19,35,47} ===\n\n");
tr=tf=0;printf("── v1 ──\n");ava(v1,"v1");bias(v1,"v1");dist(v1,"v1");col32(v1,"v1");diff(v1,"v1");spd(v1,"v1");int r1=tr,f1=tf;
tr=tf=0;printf("\n── v2 ──\n");ava(v2,"v2");bias(v2,"v2");dist(v2,"v2");col32(v2,"v2");diff(v2,"v2");spd(v2,"v2");int r2=tr,f2=tf;
printf("\nv1: %d/%d  v2: %d/%d\n",r1-f1,r1,r2-f2,r2);return(f1||f2)?1:0;}
