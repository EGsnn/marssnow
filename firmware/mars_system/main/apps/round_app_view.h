#pragma once
#include "mars_canvas.h"
/* 320x240 legacy app coordinates get circular header/safe-text placement.
 * 412x412 clients draw natively. Mapping is shared with input hit conversion. */
int round_app_x(int x,int y,int logical_w,int logical_h);
int round_app_y(int y,int logical_w,int logical_h);
bool round_app_unmap(int x,int y,int logical_w,int logical_h,uint16_t *out_x,uint16_t *out_y);
void round_app_rect(mc_canvas_t*,int,int,int,int,int,int,uint16_t);
void round_app_text(mc_canvas_t*,int,int,int,int,const char*,int,uint16_t);
int round_app_text_width(const char*,int);
void round_app_bitmap(mc_canvas_t*,int,int,int,int,int,int,int,const uint16_t*);
