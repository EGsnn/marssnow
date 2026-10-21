#pragma once
/* Shared app-side adapter. Build with MARS_APP_NATIVE_UI for kernel >=2.5.21.
 * Legacy app builds remain byte/ABI compatible; no mandatory optional import. */
#ifdef MARS_APP_NATIVE_UI
#include "mars_native_ui.h"
#include <stddef.h>
extern void gfx_clear(uint16_t);
extern void gfx_rect(int,int,int,int,uint16_t);
extern void gfx_text(int,int,const char*,int,uint16_t,uint16_t);
extern void gfx_text_overlay(int,int,const char*,int,uint16_t);
extern int gfx_text_width(const char*,int);
extern void gfx_bitmap_scaled(int,int,int,int,int,const uint16_t*);
extern void gfx_present(void);
extern int mars_display_render_emote(void);
static const mars_native_ui_v1_t *mui_ui;
static inline void mui_init(void)
{mui_ui=mars_native_ui_get(1);if(mui_ui&&mui_ui->begin(320,240)!=0)mui_ui=NULL;}
static inline void mui_clear(uint16_t c){if(mui_ui)mui_ui->clear(c);else gfx_clear(c);}
static inline void mui_rect(int x,int y,int w,int h,uint16_t c){if(mui_ui)mui_ui->rect(x,y,w,h,c);else gfx_rect(x,y,w,h,c);}
static inline void mui_text(int x,int y,const char*s,int scale,uint16_t fg,uint16_t bg){if(mui_ui)mui_ui->text(x,y,s,scale,fg);else gfx_text(x,y,s,scale,fg,bg);}
static inline void mui_overlay(int x,int y,const char*s,int scale,uint16_t fg){if(mui_ui)mui_ui->text(x,y,s,scale,fg);else gfx_text_overlay(x,y,s,scale,fg);}
static inline int mui_width(const char*s,int scale){return mui_ui?mui_ui->text_width(s,scale):gfx_text_width(s,scale);}
static inline void mui_bitmap(int x,int y,int w,int h,int scale,const uint16_t*p){if(mui_ui)mui_ui->bitmap(x,y,w,h,scale,p);else gfx_bitmap_scaled(x,y,w,h,scale,p);}
static inline void mui_present(void){if(mui_ui)mui_ui->present();else gfx_present();}
static inline int mui_emote(void){int r=mars_display_render_emote();if(r>0&&mui_ui)mui_ui->legacy_layer();return r;}
#define gfx_clear mui_clear
#define gfx_rect mui_rect
#define gfx_text mui_text
#define gfx_text_overlay mui_overlay
#define gfx_text_width mui_width
#define gfx_bitmap_scaled mui_bitmap
#define gfx_present mui_present
#define mars_display_render_emote mui_emote
#else
static inline void mui_init(void){}
#endif
