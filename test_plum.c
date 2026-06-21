#define PLUMSHASH_IMPLEMENTATION
#include "plumshash.h"
#include <stdio.h>
static inline int pop(uint64_t x){x-=x>>1&0x5555555555555555ULL;x=(x&0x3333333333333333ULL)+(x>>2&0x3333333333333333ULL);x=(x+(x>>4))&0x0F0F0F0F0F0F0F0FULL;return(x*0x0101010101010101ULL)>>56;}
int main(void){
    printf("PlumHash — quick check\n");
    /* avalanche */
    uint8_t buf[256];for(int i=0;i<256;i++)buf[i]=i*0x9D+0x37;
    uint64_t base=plumshash(buf,256,0);double lo=100;
    for(int by=0;by<32;by++)for(int bi=0;bi<8;bi++){buf[by]^=(1u<<bi);uint64_t hv=plumshash(buf,256,0);buf[by]^=(1u<<bi);double p=pop(base^hv)/64.0*100;if(p<lo)lo=p;}
    printf("Avalanche worst: %.1f%%\n",lo);
    /* chi2 */
    int bins[256]={0};for(int i=0;i<256000;i++)bins[plumshash(&i,4,i)&255]++;
    double ex=1000.0,chi2=0;for(int b=0;b<256;b++){double d=bins[b]-ex;chi2+=d*d/ex;}
    printf("Chi2: %.1f\n",chi2);
    /* sanity */
    printf("Sanity: %s\n",plumshash("hello",5,0)!=0&&plumshash("",0,0)!=plumshash("",0,42)?"PASS":"FAIL");
    printf("PlumHash ready at plumshash.h\n");
    return 0;
}
