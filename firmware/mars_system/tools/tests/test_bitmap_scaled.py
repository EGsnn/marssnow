#!/usr/bin/env python3
"""Compile the actual compositor body with a host framebuffer, without LCD SDK dependencies."""
from pathlib import Path
import subprocess
import tempfile
ROOT=Path(__file__).resolve().parents[2]
s=(ROOT/'components/mars_common/box3_gfx.c').read_text()
f=s.split('void gfx_bitmap_scaled(',1)[1].split('\nvoid gfx_frame(',1)[0]
pre='''#include <stdint.h>
#include <string.h>
#include <assert.h>
#include <limits.h>
#include <stdio.h>
#define BOX3_LCD_WIDTH 320
#define BOX3_LCD_HEIGHT 240
static uint16_t s_framebuffer[320*240];
static struct {int left,top,right,bottom;} s_clip={0,0,320,240};
'''
test='''
int main(void){
 uint16_t src[96*96];for(int i=0;i<96*96;++i)src[i]=(uint16_t)(i+1);
 int coords[][2]={{64,20},{-13,-23},{319,239},{-192,0},{320,0},{INT_MIN,INT_MAX}};
 for(int clip=0;clip<2;++clip)for(int n=0;n<6;++n){
  s_clip.left=clip?80:0;s_clip.top=clip?40:0;s_clip.right=clip?200:320;s_clip.bottom=clip?180:240;
  memset(s_framebuffer,0,sizeof(s_framebuffer));int x=coords[n][0],y=coords[n][1];
  gfx_bitmap_scaled(x,y,96,96,2,src);
  for(int py=0;py<240;++py)for(int px=0;px<320;++px){
   int64_t dx=(int64_t)px-x,dy=(int64_t)py-y;
   uint16_t expected=0;
   if(dx>=0 && dx<192 && dy>=0 && dy<192 && px>=s_clip.left && px<s_clip.right && py>=s_clip.top && py<s_clip.bottom)
    expected=src[(dy/2)*96+dx/2];
   assert(s_framebuffer[py*320+px]==expected);
  }
 }
 memset(s_framebuffer,0,sizeof(s_framebuffer));
 gfx_bitmap_scaled(0,0,INT_MAX,96,2,src);gfx_bitmap_scaled(0,0,96,96,0,src);
 gfx_bitmap_scaled(0,0,96,96,5,src);gfx_bitmap_scaled(0,0,96,96,2,NULL);
 for(int i=0;i<320*240;++i)assert(!s_framebuffer[i]);
 puts("PASS: scaled RGB565, clip, partial/off-screen and extreme coordinates under ASan/UBSan");
}
'''
with tempfile.TemporaryDirectory(prefix='mars-scaled-bitmap-') as tmp:
    p=Path(tmp)/'test.c';exe=Path(tmp)/'test'
    p.write_text(pre+'void gfx_bitmap_scaled('+f+test)
    subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror','-O1','-g','-fsanitize=address,undefined',str(p),'-o',str(exe)],check=True)
    subprocess.run([str(exe)],check=True)
