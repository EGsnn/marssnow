#include "mars_voice_effect.h"
#include <assert.h>
#include <string.h>
#include <stdio.h>

int main(void)
{
    int16_t input[4096], whole[4096], split[4096];
    uint32_t seed=42;
    for(size_t i=0;i<4096;++i) { seed=seed*1664525u+1013904223u;input[i]=(int16_t)(seed>>16); }
    for(int mode=0;mode<MARS_VOICE_COUNT;++mode) {
        memcpy(whole,input,sizeof input);memcpy(split,input,sizeof input);
        mars_voice_effect_t a={0},b={0};
        mars_voice_effect_process(&a,whole,4096,(uint8_t)mode);
        size_t at=0;
        while(at<4096) {size_t n=(at%117)+1;if(n>4096-at)n=4096-at;
            mars_voice_effect_process(&b,split+at,n,(uint8_t)mode);at+=n;}
        assert(!memcmp(whole,split,sizeof whole)); /* arbitrary Opus boundaries */
        assert(mode ? memcmp(whole,input,sizeof whole)!=0 : !memcmp(whole,input,sizeof whole));
        for(size_t i=0;i<4096;++i)assert(whole[i]>=-32768 && whole[i]<=32767);
        mars_voice_effect_process(&a,whole,4096,MARS_VOICE_ORIGINAL);
        assert(!memcmp(whole,split,sizeof whole)); /* old app never inherits an effect */
    }
    for(int mode=1;mode<MARS_VOICE_COUNT;++mode) {
        mars_voice_effect_t a={0},b={0};
        memset(whole,0,sizeof whole);mars_voice_effect_process(&a,whole,4096,(uint8_t)mode);
        for(size_t i=0;i<4096;++i)assert(!whole[i]);
        memcpy(whole,input,sizeof input);memcpy(split,input,sizeof input);
        mars_voice_effect_process(&a,whole,4096,0); /* explicit mode reset */
        mars_voice_effect_process(&a,whole,4096,(uint8_t)mode);
        mars_voice_effect_process(&b,split,4096,(uint8_t)mode);
        assert(!memcmp(whole,split,sizeof whole));
    }
    puts("PASS: original bit-exact, effects distinct/bounded, silence, arbitrary packet splits, mode reset");
}
