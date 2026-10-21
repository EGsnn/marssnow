#include "round_app_view.h"
#include <string.h>
#include <math.h>
static void fill(mc_canvas_t*c,float x,float y,float w,float h,uint16_t color)
{
 int l=(int)x,t=(int)y,r=(int)(x+w),b=(int)(y+h);
 if(l<0)l=0;
 if(t<0)t=0;
 if(r>c->width)r=c->width;
 if(b>c->height)b=c->height;
 for(int row=t;row<b;++row)for(int col=l;col<r;++col)c->pixels[row*c->width+col]=color;
}
static bool legacy(int w,int h){return w==320&&h==240;}
static float scale(int w,int h){return 412.f/(w>h?w:h);}
int round_app_x(int x,int y,int w,int h)
{if(legacy(w,h)&&y<46)return 70+x*272/320;float s=scale(w,h);return (int)((412-w*s)/2+x*s);}
int round_app_y(int y,int w,int h)
{if(legacy(w,h)&&y<46)return 62+y*48/46;float s=scale(w,h);return (int)((412-h*s)/2+y*s);}
bool round_app_unmap(int x,int y,int w,int h,uint16_t *ox,uint16_t *oy)
{
 int vx,vy;if(legacy(w,h)&&y>=62&&y<110){vx=(x-70)*320/272;vy=(y-62)*46/48;}
 else{float s=scale(w,h);vx=(int)((x-(412-w*s)/2)/s);vy=(int)((y-(412-h*s)/2)/s);}
 if(vx<0||vy<0||vx>=w||vy>=h)return false;
 *ox=vx;*oy=vy;return true;
}
void round_app_rect(mc_canvas_t*c,int w,int h,int x,int y,int rw,int rh,uint16_t color)
{
 float s=scale(w,h);
 if(rw==1&&rh==1){fill(c,(412-w*s)/2+x*s,(412-h*s)/2+y*s,1,1,color);return;}
 bool header=legacy(w,h)&&y<46&&y+rh<=46;
 int l=header?round_app_x(x,y,w,h):(int)((412-w*s)/2+x*s);
 int t=header?round_app_y(y,w,h):(int)((412-h*s)/2+y*s);
 int r=header?round_app_x(x+rw,y,w,h):(int)((412-w*s)/2+(x+rw)*s);
 int b=header?round_app_y(y+rh,w,h):(int)((412-h*s)/2+(y+rh)*s);
 if(rw>=64&&rh>=24&&rw<w)mc_round_rect(c,l,t,r-l,b-t,8,color);else fill(c,l,t,r-l,b-t,color);
}
int round_app_text_width(const char*s,int size){return (int)(mc_text_width(s,size<=1?20:20*size)*320/412);}
void round_app_text(mc_canvas_t*c,int w,int h,int x,int y,const char*s,int font,uint16_t color)
{
 int px=round_app_x(x,y,w,h),py=round_app_y(y,w,h);float size=font<=1?20:20*font;
 if(legacy(w,h)&&y<46&&(!strcmp(s,"返回")||!strcmp(s,"< BACK"))){mc_icon(c,MC_BACK,306,66,28,color);return;}
 /* Single-line status/captions retain their app's scrolling behavior. Static
  * text is kept inside the visible chord when it fits; no hidden corner label. */
 float dy=fmaxf(fabsf(py-206.f),fabsf(py+size-206));
 int inset=dy<202?206-(int)sqrtf(202*202-dy*dy):150;
 float tw=mc_text_width(s,size);
 if(x>=0&&tw<412-2*inset){if(px<inset)px=inset;if(px+tw>412-inset)px=412-inset-tw;}
 mc_text(c,px,py+size,s,size,color);
}
void round_app_bitmap(mc_canvas_t*c,int lw,int lh,int x,int y,int w,int h,int factor,const uint16_t*pixels)
{
 if(!pixels||w<=0||h<=0||factor<1)return;
 bool full=x==0&&y==0&&w*factor==lw&&h*factor==lh;
 float s=scale(lw,lh);
 float image_left=(412-lw*s)/2+x*s,image_top=(412-lh*s)/2+y*s;
 int left=full?0:(int)image_left,top=full?0:(int)image_top;
 int right=full?412:(int)(image_left+w*factor*s),bottom=full?412:(int)(image_top+h*factor*s);
 uint16_t columns[412];
 for(int px=left<0?0:left;px<right&&px<412;++px){int sx=(int)((px-image_left)/(s*factor));if(sx<0)sx=0;if(sx>=w)sx=w-1;columns[px]=(uint16_t)sx;}
 for(int py=top<0?0:top;py<bottom&&py<412;++py){int sy=(int)((py-image_top)/(s*factor));if(sy<0)sy=0;if(sy>=h)sy=h-1;
  const uint16_t *row=pixels+sy*w;uint16_t *dst=c->pixels+py*c->width;
  for(int px=left<0?0:left;px<right&&px<412;++px)dst[px]=row[columns[px]];
 }
}
