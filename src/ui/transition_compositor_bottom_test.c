/* Compare cached blitting against per-pixel composition, including alpha
 * holes, clipping and row padding. Override DS/BS/AS/RS for odd/tight strides. */
#include "src/ui/transition_compositor.h"
#include <assert.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>

#define W 4
#define H 3
#define SH 3
#ifndef DS
#define DS 12
#endif
#ifndef BS
#define BS 10
#endif
#ifndef AS
#define AS 20
#endif
#ifndef RS
#define RS 10
#endif
static uint16_t over(const uint8_t *p, uint16_t d) {
    unsigned a=p[3], inv=255-a;
    if (!a) return d;
    if (a==255) return (uint16_t)(((p[2]&0xf8)<<8)|((p[1]&0xfc)<<3)|((p[0]&0xf8)>>3));
    unsigned rr=((((p[2]>>3)*a+((d>>11)&31)*inv)<<3)&0xf800);
    unsigned gg=((((p[1]>>2)*a+((d>>5)&63)*inv)>>3)&0x07e0);
    unsigned bb=(((p[0]>>3)*a+(d&31)*inv)>>8);
    return (uint16_t)(rr|gg|bb);
}
static void check(int32_t y) {
    uint8_t base[BS*H], dst[DS*H], expected[DS*H], argb[AS*SH], cache[RS*SH];
    int32_t starts[SH]={1,2,0}, ends[SH]={3,4,0};
    memset(dst,0xa5,sizeof dst); memset(expected,0xa5,sizeof expected);
    for(int r=0;r<H;r++) for(int x=0;x<W;x++) { uint16_t c=(uint16_t)(0x1000+r*0x100+x*13); memcpy(base+r*BS+x*2,&c,2); }
    memcpy(expected, dst, sizeof dst);
    for(int r=0;r<H;r++) memcpy(expected+r*DS,base+r*BS,W*2);
    memset(argb,0,sizeof argb); memset(cache,0,sizeof cache);
    for(int r=0;r<SH;r++) for(int x=0;x<W;x++) {
        uint8_t *p=argb+r*AS+x*4; p[0]=20+x*31; p[1]=80+r*17; p[2]=220-x*20;
        p[3]=(r==0 ? (x==0?127:(x<3?255:0)) : r==1 ? (x==0?255:x==1?100:255) : (x*45));
        uint16_t c=(uint16_t)(((p[2]&0xf8)<<8)|((p[1]&0xfc)<<3)|((p[0]&0xf8)>>3)); memcpy(cache+r*RS+x*2,&c,2);
    }
    int64_t top=y<0?0:y, bottom=(int64_t)y+SH; if(bottom>H)bottom=H;
    for(int dy=(int)top;dy<bottom;dy++) for(int x=0;x<W;x++) {
        int sy=dy-y; uint16_t d; memcpy(&d,expected+dy*DS+x*2,2);
        d=over(argb+sy*AS+x*4,d);
        memcpy(expected+dy*DS+x*2,&d,2);
    }
    assert(transition_compositor_compose_bottom_sheet_rgb565(dst,DS,base,BS,argb,AS,cache,RS,starts,ends,W,H,SH,y));
    assert(memcmp(dst,expected,sizeof dst)==0);
}
int main(void) { check(-1); check(0); check(2); check(9); check(-3); check(INT32_MIN); check(INT32_MAX); puts("bottom sheet composition cases passed"); }
