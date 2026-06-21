#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#include <arm_acle.h>

static inline uint64_t rot(uint64_t x,int k){return(x<<k)|(x>>(64-k));}
#define PHI 0x9E3779B97F4A7C15ULL
#define M1 0x85EBCA77C2B2AE3DULL
#define M2 0xBF58476D1CE4E5B9ULL
#define M3 0x94D049BB133111EBULL

/* v2 baseline (multiply-based, best quality) */
uint64_t v2(const void*b,size_t l,uint64_t s){const uint8_t*p=b,*e=p+l;uint64_t ba=s^(l*PHI),h1=ba*PHI,h2=ba*M1,h3=ba*M2,h4=ba*M3;while(p+32<=e){h1^=rot(*(const uint64_t*)p,11);h1*=PHI;p+=8;h2^=rot(*(const uint64_t*)p,19);h2*=PHI;p+=8;h3^=rot(*(const uint64_t*)p,35);h3*=PHI;p+=8;h4^=rot(*(const uint64_t*)p,47);h4*=PHI;p+=8;}while(p+8<=e){h1^=rot(*(const uint64_t*)p,11);h1*=PHI;p+=8;}uint64_t t=0;switch(e-p){case 7:t^=(uint64_t)p[6]<<48;case 6:t^=(uint64_t)p[5]<<40;case 5:t^=(uint64_t)p[4]<<32;case 4:t^=(uint64_t)p[3]<<24;case 3:t^=(uint64_t)p[2]<<16;case 2:t^=(uint64_t)p[1]<<8;case 1:t^=p[0];h1^=rot(t,11);h1*=PHI;}h1^=h2;h3^=h4;h1^=h3;h1^=h1>>29;h1*=M1;h1^=h1>>31;h1*=M2;h1^=h1>>37;h1*=PHI;h1^=h1>>41;return h1;}

/* CRC variant: replace multiply with __crc32cd (1 cycle, hardware) */
uint64_t vcrc(const void*b,size_t l,uint64_t s){const uint8_t*p=b,*e=p+l;uint64_t ba=s^(l*PHI),h1=ba*PHI,h2=ba*M1,h3=ba*M2,h4=ba*M3;while(p+32<=e){h1=__crc32cd(h1^rot(*(const uint64_t*)p,11),PHI);p+=8;h2=__crc32cd(h2^rot(*(const uint64_t*)p,19),PHI);p+=8;h3=__crc32cd(h3^rot(*(const uint64_t*)p,35),PHI);p+=8;h4=__crc32cd(h4^rot(*(const uint64_t*)p,47),PHI);p+=8;}while(p+8<=e){h1=__crc32cd(h1^rot(*(const uint64_t*)p,11),PHI);p+=8;}uint64_t t=0;switch(e-p){case 7:t^=(uint64_t)p[6]<<48;case 6:t^=(uint64_t)p[5]<<40;case 5:t^=(uint64_t)p[4]<<32;case 4:t^=(uint64_t)p[3]<<24;case 3:t^=(uint64_t)p[2]<<16;case 2:t^=(uint64_t)p[1]<<8;case 1:t^=p[0];h1=__crc32cd(h1^rot(t,11),PHI);}h1^=h2;h3^=h4;h1^=h3;h1^=h1>>29;h1*=M1;h1^=h1>>31;h1*=M2;h1^=h1>>37;h1*=PHI;h1^=h1>>41;return h1;}

/* ARX variant: add+rotate+xor instead of multiply: 3 ops, all 1-cycle */
uint64_t varx(const void*b,size_t l,uint64_t s){const uint8_t*p=b,*e=p+l;uint64_t ba=s^(l*PHI),h1=ba*PHI,h2=ba*M1,h3=ba*M2,h4=ba*M3;while(p+32<=e){h1^=*(const uint64_t*)p;h1=rot(h1+h2,11);p+=8;h2^=*(const uint64_t*)p;h2=rot(h2+h3,19);p+=8;h3^=*(const uint64_t*)p;h3=rot(h3+h4,35);p+=8;h4^=*(const uint64_t*)p;h4=rot(h4+h1,47);p+=8;}while(p+8<=e){h1^=*(const uint64_t*)p;h1=rot(h1+h2,11);p+=8;}uint64_t t=0;switch(e-p){case 7:t^=(uint64_t)p[6]<<48;case 6:t^=(uint64_t)p[5]<<40;case 5:t^=(uint64_t)p[4]<<32;case 4:t^=(uint64_t)p[3]<<24;case 3:t^=(uint64_t)p[2]<<16;case 2:t^=(uint64_t)p[1]<<8;case 1:t^=p[0];h1^=t;h1=rot(h1+h2,11);}h1^=h2;h3^=h4;h1^=h3;h1^=h1>>29;h1*=M1;h1^=h1>>31;h1*=M2;h1^=h1>>37;h1*=PHI;h1^=h1>>41;return h1;}

static int pop(uint64_t x){x-=x>>1&0x5555555555555555ULL;x=(x&0x3333333333333333ULL)+(x>>2&0x3333333333333333ULL);x=(x+(x>>4))&0x0F0F0F0F0F0F0F0FULL;return(x*0x0101010101010101ULL)>>56;}

int main(void){printf("=== v2 vs CRC vs ARX ===\n\n");
    uint8_t wb[1024];volatile uint64_t ws=0;for(int i=0;i<1000;i++){ws^=v2(wb,256,i);ws^=vcrc(wb,256,i);ws^=varx(wb,256,i);}
    printf("--- SPEED ---\n%8s","");size_t szs[]={64,256,1024,8192,65536,262144};int ns=6;
    struct{const char*n;void*f;}hs[]={{"v2",v2},{"CRC",vcrc},{"ARX",varx}};int nh=3;
    for(int si=0;si<ns;si++)printf(" %8zu",szs[si]);printf("\n");
    for(int hi=0;hi<nh;hi++){printf("%-8s",hs[hi].n);typedef uint64_t(*hf)(const void*,size_t,uint64_t);hf fn=(hf)hs[hi].f;
        for(int si=0;si<ns;si++){size_t sz=szs[si];uint8_t*b=malloc(sz);for(size_t i=0;i<sz;i++)b[i]=i*0x9D+0x37;
            int it=sz<1024?50000000/sz:500000;if(it<10)it=10;volatile uint64_t s=0;clock_t st=clock();
            for(int i=0;i<it;i++)s^=fn(b,sz,i);clock_t en=clock();
            printf(" %8.1f",sz*(double)it/((double)(en-st)/CLOCKS_PER_SEC)/1e9);free(b);}printf(" GB/s\n");}
    printf("\n--- AVALANCHE ---\n");
    for(int hi=0;hi<nh;hi++){typedef uint64_t(*hf)(const void*,size_t,uint64_t);hf fn=(hf)hs[hi].f;
        uint8_t buf[256];for(int i=0;i<256;i++)buf[i]=i*0x9D+0x37;uint64_t base=fn(buf,256,0);double lo=100;
        for(int by=0;by<32;by++)for(int bi=0;bi<8;bi++){buf[by]^=(1u<<bi);uint64_t hv=fn(buf,256,0);buf[by]^=(1u<<bi);
            double p=pop(base^hv)/64.0*100;if(p<lo)lo=p;}printf("%-8s worst=%.1f%%\n",hs[hi].n,lo);}
    printf("\n--- DISTRIBUTION ---\n");
    for(int hi=0;hi<nh;hi++){typedef uint64_t(*hf)(const void*,size_t,uint64_t);hf fn=(hf)hs[hi].f;
        int bins[256]={0};for(int i=0;i<256000;i++)bins[fn(&i,4,i)&255]++;
        double ex=1000.0,chi2=0;for(int b=0;b<256;b++){double d=bins[b]-ex;chi2+=d*d/ex;}
        printf("%-8s chi2=%.1f\n",hs[hi].n,chi2);}
    return 0;
}
