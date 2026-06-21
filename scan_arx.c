/* scan_arx.c — Find best rotations for ARX (add-rotate-xor) mixing */
#include <stdio.h>
#include <stdint.h>
#include <string.h>

static inline uint64_t rot(uint64_t x,int k){return(x<<k)|(x>>(64-k));}
#define PHI 0x9E3779B97F4A7C15ULL
#define M1 0x85EBCA77C2B2AE3DULL
#define M2 0xBF58476D1CE4E5B9ULL
#define M3 0x94D049BB133111EBULL

uint64_t hash_arx(const void*b,size_t l,uint64_t s,int r1,int r2,int r3,int r4){
    const uint8_t*p=b,*e=p+l;uint64_t ba=s^(l*PHI),h1=ba*PHI,h2=ba*M1,h3=ba*M2,h4=ba*M3;
    while(p+32<=e){
        h1^=*(const uint64_t*)p;h1=rot(h1+h2,r1);p+=8;
        h2^=*(const uint64_t*)p;h2=rot(h2+h3,r2);p+=8;
        h3^=*(const uint64_t*)p;h3=rot(h3+h4,r3);p+=8;
        h4^=*(const uint64_t*)p;h4=rot(h4+h1,r4);p+=8;
    }
    while(p+8<=e){h1^=*(const uint64_t*)p;h1=rot(h1+h2,r1);p+=8;}
    uint64_t t=0;switch(e-p){case 7:t^=(uint64_t)p[6]<<48;case 6:t^=(uint64_t)p[5]<<40;case 5:t^=(uint64_t)p[4]<<32;case 4:t^=(uint64_t)p[3]<<24;case 3:t^=(uint64_t)p[2]<<16;case 2:t^=(uint64_t)p[1]<<8;case 1:t^=p[0];h1^=t;h1=rot(h1+h2,r1);}
    h1^=h2;h3^=h4;h1^=h3;h1^=h1>>29;h1*=M1;h1^=h1>>31;h1*=M2;h1^=h1>>37;h1*=PHI;h1^=h1>>41;return h1;
}
static int pop(uint64_t x){x-=x>>1&0x5555555555555555ULL;x=(x&0x3333333333333333ULL)+(x>>2&0x3333333333333333ULL);x=(x+(x>>4))&0x0F0F0F0F0F0F0F0FULL;return(x*0x0101010101010101ULL)>>56;}

int main(void){
    int cand[]={11,13,15,17,19,21,23,25,27,29,31,33,35,37,39,41,43,45,47,49,51,53,55,57,59,61};
    int nc=sizeof(cand)/sizeof(cand[0]);
    uint8_t buf[256];for(int i=0;i<256;i++)buf[i]=i*0x9D+0x37;
    int best=0,br1=0,br2=0,br3=0,br4=0;double bavg=0;

    for(int a=0;a<nc;a++){int r1=cand[a];
    for(int b=a+1;b<nc;b++){int r2=cand[b];
    for(int c=b+1;c<nc;c++){int r3=cand[c];
    for(int d=c+1;d<nc;d++){int r4=cand[d];
        if(r1+r4==64||r2+r1==64||r3+r2==64||r4+r3==64)continue; /* skip inverse rotations in chain */
        uint64_t base=hash_arx(buf,256,0,r1,r2,r3,r4);
        double s=0,lo=100;int fail=0;
        for(int by=0;by<32&&!fail;by++)for(int bi=0;bi<8&&!fail;bi++){
            buf[by]^=(1u<<bi);uint64_t hv=hash_arx(buf,256,0,r1,r2,r3,r4);buf[by]^=(1u<<bi);
            double p=pop(base^hv)/64.0*100;s+=p;if(p<lo)lo=p;if(p<20)fail=1;
        }
        if(!fail&&(int)lo>best){best=(int)lo;br1=r1;br2=r2;br3=r3;br4=r4;bavg=s/(32*8);}
        if(!fail&&lo>=35.0)printf("{%2d,%2d,%2d,%2d} worst=%.1f%%\n",r1,r2,r3,r4,lo);
    }}}}
    printf("\nBEST: {%d,%d,%d,%d} worst=%.1f%% avg=%.1f\n",br1,br2,br3,br4,(double)best,bavg);
    /* also print current {11,19,35,47} for reference */
    uint64_t base=hash_arx(buf,256,0,11,19,35,47);double lo=100,s=0;
    for(int by=0;by<32;by++)for(int bi=0;bi<8;bi++){buf[by]^=(1u<<bi);uint64_t hv=hash_arx(buf,256,0,11,19,35,47);buf[by]^=(1u<<bi);double p=pop(base^hv)/64.0*100;s+=p;if(p<lo)lo=p;}
    printf("Current {11,19,35,47} worst=%.1f%%\n",lo);
    return 0;
}
