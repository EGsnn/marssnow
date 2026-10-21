#pragma once
#include <stdint.h>
#include <stdbool.h>
typedef struct { uint16_t *pixels; int width,height; } mc_canvas_t;
void mc_clear(mc_canvas_t *,uint16_t);
void mc_pixel(mc_canvas_t *,int,int,uint16_t,unsigned);
void mc_line(mc_canvas_t *,float,float,float,float,float,uint16_t);
void mc_circle(mc_canvas_t *,float,float,float,float,uint16_t);
void mc_round_rect(mc_canvas_t *,int,int,int,int,int,uint16_t);
/* Text baseline in native pixels. ASCII uses antialiased Montserrat subsets;
 * Chinese comes from the existing shared system glyph provider. */
float mc_text_width(const char *,float);
void mc_text(mc_canvas_t *,float,float,const char *,float,uint16_t);
/* Stable subpixel grid for artwork reused at integer offsets. */
void mc_text_snap(mc_canvas_t *,float,float,const char *,float,uint16_t);
void mc_text_center(mc_canvas_t *,float,float,const char *,float,uint16_t);
void mc_arc_brand(mc_canvas_t *,float,float,float,const char *,float,uint16_t,bool);
void mc_icon(mc_canvas_t *,int,float,float,float,uint16_t);
enum { MC_WIFI,MC_BACK,MC_NEXT,MC_INFO,MC_BLE,MC_CLOCK,MC_ROBOT,MC_WAVE,MC_GRID,MC_LOCK,MC_DELETE };
