/* Host acceptance checks use the real drawer, animation and migration code. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "app_drawer.h"
#include "mars_ui_adapt.h"
mars_ui_motion_t mars_ui_motion(void) { return (mars_ui_motion_t){.fps=50, .transition_ms=180, .modal_ms=180, .ease=MARS_EASE_OUT, .modal_ease=MARS_EASE_OUT}; }
#include "box3_gfx.h"
#include "gfx_blend.h"
#include "desktop_apps_json.h"

static const char *names[10];
static int alpha, restore_count, icon_count, companion_labels, dialog_labels;
static int64_t now = 1000;
const char *mars_dynamic_bridge_package_name(int slot) { return names[slot]; }
const char *mars_dynamic_bridge_display_name(int slot) { (void)slot; return NULL; }
bool mars_dynamic_bridge_draw_icon(int slot,int x,int y) { (void)slot;(void)x;(void)y;return false; }
void gfx_clip_reset(void) {}
void gfx_capture_background(void) { icon_count = 0; }
void gfx_shade_background(uint8_t a) { assert(a > 0 && a < 255); }
void gfx_restore_background(void) { ++restore_count; }
void gfx_present_fade(uint8_t a) { alpha = a; }
void gfx_clip_push(int x,int y,int w,int h) {(void)x;(void)y;(void)w;(void)h;}
void gfx_clip_pop(void) {}
void gfx_round_rect(int x,int y,int w,int h,int r,uint16_t c)
{(void)x;(void)y;(void)r;(void)c;if(w==48 && h==48)++icon_count;}
int gfx_text_width(const char *s,int scale) {return (int)strlen(s)*6*scale;}
void gfx_text(int x,int y,const char *s,int scale,uint16_t f,uint16_t b)
{(void)x;(void)y;(void)scale;(void)f;(void)b;if(!strcmp(s,"火星伙伴"))++companion_labels;if(!strcmp(s,"流体对话"))++dialog_labels;}
void gfx_rect(int x,int y,int w,int h,uint16_t c)
{(void)x;(void)y;(void)w;(void)h;(void)c;}
void gfx_line(int x,int y,int xx,int yy,int t,uint16_t c)
{(void)x;(void)y;(void)xx;(void)yy;(void)t;(void)c;}

static void finish_fade(bool opening)
{
    int previous = alpha;
    for(int i=0;i<=9;++i) {
        app_drawer_tick(now+i*20);
        assert(opening ? alpha>=previous : alpha<=previous);
        previous=alpha;
    }
    now+=200;
    assert(alpha==(opening ? 255 : 0));
}
static void tap(int x,int y)
{ app_drawer_touch(true,x,y); app_drawer_touch(false,0,0); }
static void ready(void)
{
    app_drawer_open();
    /* The opening finger remains down past the animation: ignore until up. */
    app_drawer_touch(true,100,80);
    finish_fade(true);
    app_drawer_touch(true,100,80);
    app_drawer_touch(false,0,0);
    assert(app_drawer_is_open() && app_drawer_take_launch()==-1);
}
static void closed(int expected)
{
    assert(app_drawer_is_open()); /* close is animated */
    finish_fade(false);
    assert(!app_drawer_is_open());
    assert(app_drawer_take_launch()==expected && app_drawer_take_launch()==-1);
}
static void drawer_tests(void)
{
    ready(); assert(icon_count==0); tap(76,100); assert(app_drawer_is_open());
    tap(10,120); closed(-1);
    names[0]="xiaozhi"; names[2]="test_app";
    ready(); assert(icon_count==2);
    tap(160,100); closed(2); /* holes in the slot list preserve slot identity */
    ready(); tap(76,146); closed(0); /* labels are clickable too */
    names[1]="mars_companion";
    ready(); assert(icon_count==3 && companion_labels==1); tap(244,100); closed(2);
    names[2]="fluid_dialog";
    ready();assert(icon_count==3&&dialog_labels==1);tap(244,146);closed(2);
    ready();
    app_drawer_touch(true,100,80); app_drawer_touch(true,100,100);
    app_drawer_touch(false,0,0);
    assert(app_drawer_take_launch()==-1 && app_drawer_is_open());
    tap(118,100); assert(app_drawer_is_open()); /* gap between application cells */
    tap(244,190); assert(app_drawer_is_open()); /* blank below icons */
    tap(268,42); closed(-1);
    names[3]="ocean_clock";
    ready(); assert(icon_count==3);
    tap(245,195); assert(app_drawer_is_open() && icon_count==4);
    tap(76,100); closed(3); /* fourth app opens from page two */
    ready();
    app_drawer_touch(true,255,115); app_drawer_touch(true,190,116);
    app_drawer_touch(false,0,0);
    assert(icon_count==4);
    app_drawer_touch(true,65,115); app_drawer_touch(true,135,116);
    app_drawer_touch(false,0,0);
    assert(icon_count==7);
    tap(76,100); closed(0);
    names[3]=NULL;
    ready(); app_drawer_close(); app_drawer_close(); closed(-1);
    app_drawer_open(); app_drawer_close(); finish_fade(false);
    assert(!app_drawer_is_open() && restore_count==10);
    names[0]=NULL; names[1]=NULL;
    ready(); assert(icon_count==1); tap(76,100); closed(2);
    for(int a=0;a<=255;++a) {
        uint16_t pixel=gfx_blend_rgb565(0xffff,0,(uint8_t)a);
        assert(pixel==gfx_blend_rgb565(0,0xffff,(uint8_t)(255-a)));
    }
    assert(gfx_blend_rgb565(0x1234,0xabcd,0)==0x1234);
    assert(gfx_blend_rgb565(0x1234,0xabcd,255)==0xabcd);
}
static const char *action(cJSON *node)
{
    return cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(node,"onTap"),"actionId")->valuestring;
}
static void migration_tests(void)
{
    cJSON *root=cJSON_Parse("{\"version\":2,\"entryPage\":\"home\",\"pages\":[{\"id\":\"home\",\"elements\":[{\"type\":\"container\",\"children\":[{\"type\":\"button\",\"icon\":\"home\",\"onTap\":{\"type\":\"invoke\",\"actionId\":\"app.xiaozhi.open\"}}]},{\"type\":\"button\",\"onTap\":{\"type\":\"invoke\",\"actionId\":\"app.dyn.0.open\"}},{\"type\":\"button\",\"onTap\":{\"type\":\"invoke\",\"actionId\":\"app.clock.open\"}}]},{\"id\":\"other\",\"elements\":[{\"type\":\"button\",\"onTap\":{\"type\":\"invoke\",\"actionId\":\"app.xiaozhi.open\"}}]}]}");
    assert(root);
    desktop_apps_json_normalize(root);
    cJSON *pages=cJSON_GetObjectItemCaseSensitive(root,"pages");
    cJSON *elements=cJSON_GetObjectItemCaseSensitive(cJSON_GetArrayItem(pages,0),"elements");
    assert(cJSON_GetArraySize(elements)==2);
    cJSON *button=cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(cJSON_GetArrayItem(elements,0),"children"),0);
    assert(strcmp(action(button),"app.apps.open")==0);
    assert(strcmp(cJSON_GetObjectItemCaseSensitive(button,"icon")->valuestring,"apps")==0);
    assert(!cJSON_GetObjectItemCaseSensitive(button,"text"));
    assert(strcmp(action(cJSON_GetArrayItem(elements,1)),"app.clock.open")==0);
    cJSON *other=cJSON_GetObjectItemCaseSensitive(cJSON_GetArrayItem(pages,1),"elements");
    assert(strcmp(action(cJSON_GetArrayItem(other,0)),"app.xiaozhi.open")==0);
    char *before=cJSON_PrintUnformatted(root);
    desktop_apps_json_normalize(root);
    char *after=cJSON_PrintUnformatted(root);
    assert(strcmp(before,after)==0); free(before);free(after);cJSON_Delete(root);
    root=cJSON_Parse("{\"version\":1,\"screen\":{},\"elements\":[{\"type\":\"button\",\"onTap\":{\"type\":\"invoke\",\"actionId\":\"app.dyn.0.open\"}}]}");
    desktop_apps_json_normalize(root);
    assert(strcmp(action(cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(root,"elements"),0)),"app.apps.open")==0);
    cJSON_Delete(root);
    root=cJSON_Parse("{\"version\":2,\"entryPage\":\"home\",\"pages\":[{\"id\":\"home\",\"elements\":[{\"type\":\"button\",\"text\":\"APP应用\",\"onTap\":{\"type\":\"invoke\",\"actionId\":\"app.apps.open\"}}]}]}");
    desktop_apps_json_normalize(root);
    button=cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(root,"pages"),0),"elements"),0);
    assert(!cJSON_GetObjectItemCaseSensitive(button,"text"));
    assert(strcmp(cJSON_GetObjectItemCaseSensitive(button,"icon")->valuestring,"apps")==0);
    cJSON_Delete(root);
    root=cJSON_Parse("{\"version\":2,\"pages\":[]}"); desktop_apps_json_normalize(root);cJSON_Delete(root);
    root=cJSON_Parse("{\"version\":2,\"entryPage\":\"home\",\"pages\":[{\"id\":\"home\",\"elements\":[{\"type\":\"text\",\"text\":\"keep me\"},{\"type\":\"block\",\"x\":12,\"y\":198,\"width\":296,\"height\":36},{\"type\":\"button\",\"y\":203,\"width\":28,\"height\":28,\"onTap\":{\"type\":\"invoke\",\"actionId\":\"app.apps.open\"}},{\"type\":\"button\",\"y\":203,\"width\":28,\"height\":28,\"onTap\":{\"type\":\"invoke\",\"actionId\":\"app.clock.open\"}},{\"type\":\"button\",\"y\":203,\"width\":28,\"height\":28,\"onTap\":{\"type\":\"invoke\",\"actionId\":\"app.timer.open\"}},{\"type\":\"button\",\"y\":203,\"width\":28,\"height\":28,\"onTap\":{\"type\":\"invoke\",\"actionId\":\"app.system.open\"}}]},{\"id\":\"other\",\"elements\":[{\"type\":\"button\",\"onTap\":{\"type\":\"invoke\",\"actionId\":\"app.timer.open\"}}]}]}");
    assert(root);desktop_apps_json_normalize(root);
    pages=cJSON_GetObjectItemCaseSensitive(root,"pages");
    elements=cJSON_GetObjectItemCaseSensitive(cJSON_GetArrayItem(pages,0),"elements");
    assert(cJSON_GetArraySize(elements)==6);
    assert(!strcmp(cJSON_GetObjectItemCaseSensitive(cJSON_GetArrayItem(elements,0),"text")->valuestring,"keep me"));
    assert(!strcmp(action(cJSON_GetArrayItem(elements,3)),"app.apps.open"));
    assert(!strcmp(action(cJSON_GetArrayItem(elements,4)),"app.fluid_clock.open"));
    assert(!strcmp(action(cJSON_GetArrayItem(elements,5)),"app.system.open"));
    other=cJSON_GetObjectItemCaseSensitive(cJSON_GetArrayItem(pages,1),"elements");
    assert(!strcmp(action(cJSON_GetArrayItem(other,0)),"app.timer.open"));
    before=cJSON_PrintUnformatted(root);desktop_apps_json_normalize(root);after=cJSON_PrintUnformatted(root);
    assert(!strcmp(before,after));free(before);free(after);cJSON_Delete(root);
    root=cJSON_Parse("{\"version\":1,\"elements\":[{\"type\":\"button\",\"y\":203,\"width\":28,\"height\":28,\"onTap\":{\"type\":\"invoke\",\"actionId\":\"app.clock.open\"}}]}");
    desktop_apps_json_normalize(root);
    assert(!strcmp(action(cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(root,"elements"),0)),"app.clock.open"));cJSON_Delete(root);
}
int main(void)
{
    drawer_tests(); migration_tests();
    puts("PASS: drawer gestures, sparse slots, fade endpoints, RGB565 blend, saved desktop migration");
    return 0;
}
