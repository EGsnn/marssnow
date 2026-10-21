#include "mars_canvas.h"
#include "mars_glyph.h"
#include "round_fonts.h"
#include <math.h>
#include <stddef.h>
#include <string.h>
static float clamp(float v,float lo,float hi){return v<lo?lo:v>hi?hi:v;}
void mc_clear(mc_canvas_t *c,uint16_t color){for(int i=0;i<c->width*c->height;++i)c->pixels[i]=color;}
void mc_pixel(mc_canvas_t *c,int x,int y,uint16_t color,unsigned a)
{
 if(x<0||y<0||x>=c->width||y>=c->height||!a)return;
 uint16_t *p=c->pixels+y*c->width+x;if(a>=255){*p=color;return;}
 unsigned b=*p,ia=255-a;
 *p=(uint16_t)((((((color>>11)&31)*a+((b>>11)&31)*ia+127)/255)<<11)|
 (((((color>>5)&63)*a+((b>>5)&63)*ia+127)/255)<<5)|(((color&31)*a+(b&31)*ia+127)/255));
}
void mc_line(mc_canvas_t *c,float x,float y,float ex,float ey,float width,uint16_t color)
{
 float dx=ex-x,dy=ey-y,len=dx*dx+dy*dy,r=width*.5f;
 int l=(int)floorf(fminf(x,ex)-r-1),t=(int)floorf(fminf(y,ey)-r-1);
 int right=(int)ceilf(fmaxf(x,ex)+r+1),bottom=(int)ceilf(fmaxf(y,ey)+r+1);
 for(int py=t;py<bottom;++py)for(int px=l;px<right;++px){
  float at=len?clamp(((px-x)*dx+(py-y)*dy)/len,0,1):0;
  float xx=px-x-at*dx,yy=py-y-at*dy;
  mc_pixel(c,px,py,color,(unsigned)(255*clamp(r+.5f-sqrtf(xx*xx+yy*yy),0,1)));
 }
}
void mc_circle(mc_canvas_t *c,float x,float y,float radius,float width,uint16_t color)
{
 float outer=radius+(width>0?width*.5f:0);
 float support=outer+.5f,inner=width>0?fmaxf(0,radius-width*.5f-.5f):0;
 for(int py=(int)(y-outer-1);py<=y+outer+1;++py)for(int px=(int)(x-outer-1);px<=x+outer+1;++px){
  float dx=px-x,dy=py-y,d2=dx*dx+dy*dy;
  if(d2>support*support||(inner>0&&d2<inner*inner))continue;
  float d=hypotf(dx,dy);float a=width>0?width*.5f+.5f-fabsf(d-radius):radius+.5f-d;
  mc_pixel(c,px,py,color,(unsigned)(255*clamp(a,0,1)));
 }
}
void mc_round_rect(mc_canvas_t *c,int x,int y,int w,int h,int r,uint16_t color)
{
 for(int py=0;py<h;++py)for(int px=0;px<w;++px){
  float dx=fmaxf(fabsf(px-(w-1)*.5f)-(w*.5f-r),0),dy=fmaxf(fabsf(py-(h-1)*.5f)-(h*.5f-r),0);
  mc_pixel(c,x+px,y+py,color,(unsigned)(255*clamp(r+.5f-hypotf(dx,dy),0,1)));
 }
}
static unsigned utf8(const char **s){unsigned v=(unsigned char)*(*s)++;if(v<128)return v;int n=(v&0xe0)==0xc0?1:(v&0xf0)==0xe0?2:(v&0xf8)==0xf0?3:0;v&=(1u<<(6-n))-1;while(n--&&**s)v=(v<<6)|((unsigned char)*(*s)++&63);return v;}
static const mc_glyph_t *glyph(unsigned cp,float size,const unsigned char **data,float *scale)
{
 int px=size>32?48:size>17?20:16;*scale=size/px;
 *data=px==48?mc_font_48:px==20?mc_font_20:mc_font_16;
 if(cp<32||cp>126)cp='?';
 return (px==48?mc_glyph_48:px==20?mc_glyph_20:mc_glyph_16)+(cp-32);
}
static float advance(unsigned cp,float size){const unsigned char *d;float scale;const mc_glyph_t *g=glyph(cp,size,&d,&scale);return cp>=128?size:g->advance*scale/16;}
float mc_text_width(const char *s,float size){float w=0;while(*s)w+=advance(utf8(&s),size);return w;}
static void character(mc_canvas_t *c,float cx,float baseline,unsigned cp,float size,float angle,uint16_t color,bool snap)
{
 /* Subpixel positions must stay invariant under integer canvas translations. */
 if(snap){cx=roundf(cx*256.0f)/256.0f;baseline=roundf(baseline*256.0f)/256.0f;}
 const unsigned char *data;float scale;const mc_glyph_t *g=glyph(cp,size,&data,&scale);
 uint8_t cn[32];uint32_t cnw=16;bool chinese=cp>=128;
 int w=chinese?16:g->w,h=chinese?16:g->h;
 if(chinese){mars_glyph_copy(cp,cn,sizeof(cn),&cnw);scale=size/16;}
 float adv=advance(cp,size),left=chinese?-size*.5f:g->x*scale-adv*.5f;
 float top=chinese?-size:-(g->h+g->y)*scale;
 float cosine=cosf(angle),sine=sinf(angle);
 /* Bilinear glyph support is [-1,w] x [-1,h]. Transform its four corners
  * once instead of filtering a size*3.2 square for every character. */
 float minx=INFINITY,miny=INFINITY,maxx=-INFINITY,maxy=-INFINITY;
 for(int iy=0;iy<2;++iy)for(int ix=0;ix<2;++ix){
  float u=left+(ix?w:-1)*scale,v=top+(iy?h:-1)*scale;
  float px=cx+u*cosine-v*sine,py=baseline+u*sine+v*cosine;
  minx=fminf(minx,px);maxx=fmaxf(maxx,px);miny=fminf(miny,py);maxy=fmaxf(maxy,py);
 }
 int l=(int)floorf(fmaxf(0,minx))-1,t=(int)floorf(fmaxf(0,miny))-1;
 int r=(int)ceilf(fminf(c->width-1,maxx))+1,b=(int)ceilf(fminf(c->height-1,maxy))+1;
 for(int y=t;y<=b;++y)for(int x=l;x<=r;++x){
  float dx=x-cx,dy=y-baseline;
  float sx=(dx*cosine+dy*sine-left)/scale,sy=(-dx*sine+dy*cosine-top)/scale;
  int ix=(int)floorf(sx),iy=(int)floorf(sy);float a=0;
  for(int yy=0;yy<2;++yy)for(int xx=0;xx<2;++xx){int px=ix+xx,py=iy+yy;
   if(px<0||py<0||px>=w||py>=h)continue;
   unsigned value;if(chinese)value=(cn[py*2+px/8]&(0x80>>(px%8)))?15:0;
   else{unsigned i=py*w+px;value=(data[g->offset+i/2]>>(i%2?0:4))&15;}
   float weight=(xx?sx-ix:1-(sx-ix))*(yy?sy-iy:1-(sy-iy));a+=value*weight;
  }
  if(a>0)mc_pixel(c,x,y,color,(unsigned)clamp(a*17,0,255));
 }
}
void mc_text(mc_canvas_t *c,float x,float base,const char *s,float size,uint16_t color){while(*s){unsigned cp=utf8(&s);float adv=advance(cp,size);character(c,x+adv*.5f,base,cp,size,0,color,false);x+=adv;}}
void mc_text_snap(mc_canvas_t *c,float x,float base,const char *s,float size,uint16_t color){while(*s){unsigned cp=utf8(&s);float adv=advance(cp,size);character(c,x+adv*.5f,base,cp,size,0,color,true);x+=adv;}}
void mc_text_center(mc_canvas_t *c,float x,float base,const char *s,float size,uint16_t color){mc_text(c,x-mc_text_width(s,size)*.5f,base,s,size,color);}
void mc_arc_brand(mc_canvas_t *c,float cx,float cy,float radius,const char *s,float size,uint16_t color,bool wifi)
{
 float width=mc_text_width(s,size),spacing=3.2f;int n=(int)strlen(s);width+=(n-1)*spacing;
 float pos=-width*.5f;
 if(wifi){float a=(pos-19)/radius;float x=cx+radius*sinf(a),y=cy-radius*cosf(a);
  /* Rotate Wi-Fi segments around its baseline using the same arc tangent. */
  for(int ring=0;ring<3;++ring){float rr=4+ring*4;for(int i=0;i<16;++i){float t0=(-130+i*80.f/16)*.0174532925f,t1=(-130+(i+1)*80.f/16)*.0174532925f;
   float xx=cosf(t0)*rr,yy=sinf(t0)*rr,ex=cosf(t1)*rr,ey=sinf(t1)*rr;
   mc_line(c,x+xx*cosf(a)-yy*sinf(a),y+xx*sinf(a)+yy*cosf(a),x+ex*cosf(a)-ey*sinf(a),y+ex*sinf(a)+ey*cosf(a),1.3f,color);
  }}mc_circle(c,x,y,1,0,color);
 }
 for(int i=0;i<n;++i){float adv=advance((unsigned char)s[i],size),a=(pos+adv*.5f)/radius;
  character(c,cx+radius*sinf(a),cy-radius*cosf(a),(unsigned char)s[i],size,a,color,false);pos+=adv+spacing;}
}
void mc_icon(mc_canvas_t *c,int type,float x,float y,float size,uint16_t color)
{
 float k=size/22;
 #define L(a,b,d,e) mc_line(c,x+(a)*k,y+(b)*k,x+(d)*k,y+(e)*k,1.5f,color)
 #define C(a,b,r) mc_circle(c,x+(a)*k,y+(b)*k,(r)*k,1.5f,color)
 if(type==MC_BACK){L(13,5,6,11);L(6,11,13,17);L(6,11,19,11);}
 else if(type==MC_NEXT){L(9,6,14,11);L(14,11,9,16);}
 else if(type==MC_INFO){C(11,11,8);L(11,10,11,16);C(11,6,.4f);}
 else if(type==MC_CLOCK){C(11,11,9);L(11,5,11,11);L(11,11,15,13);}
 else if(type==MC_BLE){L(7,5,17,17);L(17,17,11,21);L(11,21,11,1);L(11,1,17,5);L(17,5,7,17);}
 else if(type==MC_WIFI){for(int ring=0;ring<3;++ring){float r=5+ring*5;for(int i=0;i<20;++i){float a=(-130+i*4)*.0174532925f,b=a+.06981317f;L(11+cosf(a)*r,20+sinf(a)*r,11+cosf(b)*r,20+sinf(b)*r);}}C(11,20,.6f);}
 else if(type==MC_GRID){for(int i=0;i<4;++i){int a=3+(i%2)*10,b=3+(i/2)*10;L(a,b,a+6,b);L(a+6,b,a+6,b+6);L(a+6,b+6,a,b+6);L(a,b+6,a,b);}}
 else if(type==MC_LOCK){L(5,10,17,10);L(5,10,5,20);L(17,10,17,20);L(5,20,17,20);L(7,10,7,5);L(7,5,15,5);L(15,5,15,10);}
 else if(type==MC_ROBOT){L(3,6,19,6);L(3,6,3,18);L(19,6,19,18);L(3,18,19,18);L(11,2,11,6);C(8,11,.6);C(14,11,.6);L(8,14,14,14);}
 else if(type==MC_DELETE){L(8,5,20,5);L(20,5,20,17);L(20,17,8,17);L(8,17,2,11);L(2,11,8,5);L(11,8,17,14);L(17,8,11,14);}
 else{for(int row=0;row<3;++row)for(int i=0;i<20;++i){float a=i*.314159f,b=(i+1)*.314159f;L(2+i*.9f,6+row*5+sinf(a)*2,2+(i+1)*.9f,6+row*5+sinf(b)*2);}}
 #undef L
 #undef C
}
