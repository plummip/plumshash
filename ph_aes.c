#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#include <arm_neon.h>

/* AES-based mix: 1 round of AES encryption = extremely fast, great diffusion */
static inline uint8x16_t aes_mix(uint8x16_t state, uint8x16_t data) {
    /* XOR data into state, then AES encrypt round */
    state = veorq_u8(state, data);
    /* AESE + AESMC = full AES round (SubBytes+ShiftRows+MixColumns) */
    /* On aarch64 this is a single instruction pair: aese + aesmc */
    uint8x16_t out;
    __asm__("aese %0.16b, %1.16b" : "=w"(out) : "w"(state));
    __asm__("aesmc %0.16b, %0.16b" : "+w"(out));
    return out;
}

/* AES-based hash: 128-bit state, 16 bytes per iteration */
uint64_t aeshash(const void *b, size_t l, uint64_t seed) {
    const uint8_t *p = (const uint8_t *)b;
    const uint8_t *e = p + l;

    /* Initialize 128-bit state from seed */
    uint8x16_t state = vcombine_u8(
        vcreate_u8(seed ^ (l * 0x9E3779B97F4A7C15ULL)),
        vcreate_u8((~seed) ^ 0xBF58476D1CE4E5B9ULL)
    );

    /* Full 16-byte blocks */
    while (p + 16 <= e) {
        uint8x16_t data = vld1q_u8(p);
        state = aes_mix(state, data);
        p += 16;
    }

    /* Tail: pad with zeros and process */
    if (p < e) {
        uint8_t tail[16] = {0};
        memcpy(tail, p, e - p);
        uint8x16_t data = vld1q_u8(tail);
        state = aes_mix(state, data);
    }

    /* Finalize: 2 extra AES rounds */
    uint8x16_t zero = vdupq_n_u8(0);
    state = aes_mix(state, zero);
    state = aes_mix(state, zero);

    /* Extract 64-bit result from low 64 bits of state */
    return vgetq_lane_u64(vreinterpretq_u64_u8(state), 0) ^
           vgetq_lane_u64(vreinterpretq_u64_u8(state), 1);
}

/* Reference: PrimeHash v2 for comparison */
static inline uint64_t rot(uint64_t x,int k){return(x<<k)|(x>>(64-k));}
#define PHI 0x9E3779B97F4A7C15ULL
#define M1 0x85EBCA77C2B2AE3DULL
#define M2 0xBF58476D1CE4E5B9ULL
#define M3 0x94D049BB133111EBULL
uint64_t ph_v2(const void*b,size_t l,uint64_t s){const uint8_t*p=b,*e=p+l;uint64_t ba=s^(l*PHI),h1=ba*PHI,h2=ba*M1,h3=ba*M2,h4=ba*M3;while(p+32<=e){h1^=rot(*(const uint64_t*)p,11);h1*=PHI;p+=8;h2^=rot(*(const uint64_t*)p,19);h2*=PHI;p+=8;h3^=rot(*(const uint64_t*)p,35);h3*=PHI;p+=8;h4^=rot(*(const uint64_t*)p,47);h4*=PHI;p+=8;}while(p+8<=e){h1^=rot(*(const uint64_t*)p,11);h1*=PHI;p+=8;}uint64_t t=0;switch(e-p){case 7:t^=(uint64_t)p[6]<<48;case 6:t^=(uint64_t)p[5]<<40;case 5:t^=(uint64_t)p[4]<<32;case 4:t^=(uint64_t)p[3]<<24;case 3:t^=(uint64_t)p[2]<<16;case 2:t^=(uint64_t)p[1]<<8;case 1:t^=p[0];h1^=rot(t,11);h1*=PHI;}h1^=h2;h3^=h4;h1^=h3;h1^=h1>>29;h1*=M1;h1^=h1>>31;h1*=M2;h1^=h1>>37;h1*=PHI;h1^=h1>>41;return h1;}

static int pop(uint64_t x){x-=x>>1&0x5555555555555555ULL;x=(x&0x3333333333333333ULL)+(x>>2&0x3333333333333333ULL);x=(x+(x>>4))&0x0F0F0F0F0F0F0F0FULL;return(x*0x0101010101010101ULL)>>56;}

int main(void){
    printf("=== AES-Hash vs PrimeHash v2 ===\n\n");

    /* Warmup */
    uint8_t wb[1024]; volatile uint64_t ws=0;
    for(int i=0;i<1000;i++){ws^=aeshash(wb,256,i);ws^=ph_v2(wb,256,i);}

    /* Speed */
    printf("--- SPEED (GB/s) ---\n%8s","");
    size_t szs[]={16,64,256,1024,8192,65536,262144,1048576};int ns=8;
    struct{const char*n;void*f;}hs[]={{"AES",aeshash},{"PHv2",ph_v2}};int nh=2;
    for(int si=0;si<ns;si++)printf(" %8zu",szs[si]);printf("\n");
    for(int hi=0;hi<nh;hi++){printf("%-8s",hs[hi].n);
        typedef uint64_t(*hf)(const void*,size_t,uint64_t);hf fn=(hf)hs[hi].f;
        for(int si=0;si<ns;si++){size_t sz=szs[si];uint8_t*b=malloc(sz);for(size_t i=0;i<sz;i++)b[i]=i*0x9D+0x37;
            int it=sz<1024?50000000/sz:500000;if(it<10)it=10;volatile uint64_t s=0;clock_t st=clock();
            for(int i=0;i<it;i++)s^=fn(b,sz,i);clock_t en=clock();
            printf(" %8.1f",sz*(double)it/((double)(en-st)/CLOCKS_PER_SEC)/1e9);free(b);}printf(" GB/s\n");}

    /* Quality: avalanche */
    printf("\n--- AVALANCHE (worst%%) ---\n");
    for(int hi=0;hi<nh;hi++){typedef uint64_t(*hf)(const void*,size_t,uint64_t);hf fn=(hf)hs[hi].f;
        uint8_t buf[256];for(int i=0;i<256;i++)buf[i]=i*0x9D+0x37;uint64_t base=fn(buf,256,0);double lo=100;
        for(int by=0;by<32;by++)for(int bi=0;bi<8;bi++){buf[by]^=(1u<<bi);uint64_t hv=fn(buf,256,0);buf[by]^=(1u<<bi);
            double p=pop(base^hv)/64.0*100;if(p<lo)lo=p;}
        printf("%-8s worst=%.1f%%\n",hs[hi].n,lo);}

    /* Quality: chi-squared */
    printf("\n--- CHI2 ---\n");
    for(int hi=0;hi<nh;hi++){typedef uint64_t(*hf)(const void*,size_t,uint64_t);hf fn=(hf)hs[hi].f;
        int bins[256]={0};for(int i=0;i<256000;i++)bins[fn(&i,4,i)&255]++;
        double expv=1000.0,chi2=0;for(int b=0;b<256;b++){double d=bins[b]-expv;chi2+=d*d/expv;}
        printf("%-8s chi2=%.1f\n",hs[hi].n,chi2);}
    return 0;
}
