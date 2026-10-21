#include "mars_layout.h"
#include <string.h>
#include <stdint.h>
static int pixels(int units,int density){return (units*density+255)/256;}
static int smaller(int a,int b){return a<b?a:b;}
bool mars_layout_solve(mars_layout_spec_t s,mars_layout_plan_t *out)
{
    if(!out)return false;
    memset(out,0,sizeof(*out));
    if(s.width<1||s.height<1||s.width>4096||s.height>4096||s.inset<0||
       s.inset>=s.width/2||s.inset>=s.height/2||s.density_q8<128||s.density_q8>1024||
       (s.round&&s.width!=s.height))return false;
    int width=s.width-2*s.inset,height=s.height-2*s.inset;
    if(s.round){width=height=width*181/256;}
    mars_layout_plan_t p={.frame={(s.width-width)/2,(s.height-height)/2,width,height},.density_q8=s.density_q8};
    int wu=width*256/s.density_q8,hu=height*256/s.density_q8;
    p.tier=wu<360||hu<240?MARS_LAYOUT_COMPACT:wu<600?MARS_LAYOUT_REGULAR:MARS_LAYOUT_EXPANDED;
    p.gap=pixels(p.tier==MARS_LAYOUT_COMPACT?8:p.tier==MARS_LAYOUT_REGULAR?12:16,s.density_q8);
    p.touch_min=pixels(40,s.density_q8);
    p.body_font=pixels(p.tier==MARS_LAYOUT_COMPACT?16:p.tier==MARS_LAYOUT_REGULAR?18:20,s.density_q8);
    p.title_font=pixels(p.tier==MARS_LAYOUT_COMPACT?24:p.tier==MARS_LAYOUT_REGULAR?26:28,s.density_q8);
    int bar=p.touch_min+2*p.gap;
    if(width<p.touch_min||height<bar+p.gap+p.touch_min+(s.footer?bar+p.gap:0))return false;
    p.header=(mars_layout_rect_t){p.frame.x,p.frame.y,width,bar};
    p.body=(mars_layout_rect_t){p.frame.x,p.frame.y+bar+p.gap,width,height-bar-p.gap-(s.footer?bar+p.gap:0)};
    if(s.footer)p.footer=(mars_layout_rect_t){p.frame.x,p.frame.y+height-bar,width,bar};
    *out=p;return true;
}
bool mars_layout_grid(const mars_layout_plan_t*p,int minw,int minh,int maxcols,int count,mars_layout_grid_t*out)
{
    if(!out)return false;
    memset(out,0,sizeof(*out));
    if(!p||p->density_q8<128||p->density_q8>1024||p->gap<0||p->gap>512||p->touch_min<1||p->touch_min>4096||
       p->body.x<0||p->body.y<0||p->body.width<1||p->body.width>4096||p->body.height<1||p->body.height>4096||
       minw<1||minh<1||minw>4096||minh>4096||maxcols<1||maxcols>12||count<1||count>256)return false;
    int width=pixels(minw,p->density_q8),height=pixels(minh,p->density_q8);
    if(width<p->touch_min)width=p->touch_min;
    if(height<p->touch_min)height=p->touch_min;
    int columns=smaller(maxcols,(p->body.width+p->gap)/(width+p->gap));
    int rows=(p->body.height+p->gap)/(height+p->gap);
    if(!columns||!rows)return false;
    columns=smaller(columns,count);
    int capacity=columns*rows,visible=smaller(count,capacity);
    rows=(visible+columns-1)/columns;
    *out=(mars_layout_grid_t){p->body,columns,rows,(p->body.width-(columns-1)*p->gap)/columns,
        (p->body.height-(rows-1)*p->gap)/rows,p->gap,visible,(count+capacity-1)/capacity};
    return true;
}
bool mars_layout_cell(const mars_layout_grid_t*g,int index,mars_layout_rect_t*out)
{
    if(!out)return false;
    *out=(mars_layout_rect_t){0};
    if(!g||g->columns<1||g->columns>12||g->rows<1||g->rows>4096||
       g->cell_width<1||g->cell_width>4096||g->cell_height<1||g->cell_height>4096||
       g->gap<0||g->gap>512||g->area.x<0||g->area.y<0||g->area.x>4096||g->area.y>4096||
       g->area.width<1||g->area.width>4096||g->area.height<1||g->area.height>4096||
       g->visible_count<1||g->visible_count>256||index<0||index>=g->visible_count)return false;
    *out=(mars_layout_rect_t){g->area.x+(index%g->columns)*(g->cell_width+g->gap),
        g->area.y+(index/g->columns)*(g->cell_height+g->gap),g->cell_width,g->cell_height};
    if(out->x+out->width>g->area.x+g->area.width||out->y+out->height>g->area.y+g->area.height){*out=(mars_layout_rect_t){0};return false;}
    return true;
}
bool mars_layout_hit(mars_layout_rect_t r,int x,int y)
{int64_t dx=(int64_t)x-r.x,dy=(int64_t)y-r.y;return r.width>0&&r.height>0&&dx>=0&&dy>=0&&dx<r.width&&dy<r.height;}
