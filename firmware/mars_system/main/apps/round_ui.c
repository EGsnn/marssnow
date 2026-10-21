/* Native system shell. The launcher remains the display/lifecycle owner.
 * This controller never creates LCD/I2C/audio/serial resources or an App task. */
#include "round_ui.h"
#include "round_view.h"
#include "round_app_view.h"
#include "mars_native_ui.h"
#include "box3_gfx.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mars_board.h"
#include "mars_ui_adapt.h"
#include "mars_gesture.h"
#include "mars_clock.h"
#include "mars_dynamic_bridge.h"
#include "mars_app_loader.h"
#include "settings_wifi.h"
#include "settings_bt.h"
#include "esp_heap_caps.h"
#include "esp_attr.h"
#include "esp_app_desc.h"
#include "esp_log.h"
#include "esp_timer.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

static const char *TAG="round_ui";
EXT_RAM_BSS_ATTR static round_model_t m;
static mc_canvas_t canvas;
static uint16_t *previous;
static bool app_snapshot,native_app;
static int app_width=320,app_height=240;
static bool active,dirty,down,wait_up,animating;
static int animation_sign;
static int64_t animation_start,last_refresh;
static unsigned animation_duration,animation_frames;
static uint64_t animation_draw_us;
static uint32_t animation_max_us;
static char pending_action[40],taken_action[40];
static mars_input_state_t input;
static mars_gesture_t gestures;
static void copy(char*to,size_t size,const char*from){if(!size)return;if(!from)from="";size_t n=strlen(from);if(n>=size)n=size-1;memmove(to,from,n);to[n]=0;}
static const char *wifi_status(void){switch(settings_wifi_state()){case WIFI_SET_CONNECTED:return "已连接";case WIFI_SET_CONNECTING:return "连接中";case WIFI_SET_FAILED:return "连接失败";default:return "未连接";}}
static int package_icon(const char*name){if(!name)return MC_GRID;if(strstr(name,"clock"))return MC_CLOCK;if(strstr(name,"xiaozhi"))return MC_ROBOT;if(strstr(name,"fluid")||strstr(name,"ocean"))return MC_WAVE;return MC_GRID;}
static uint32_t model_hash(void)
{
 const unsigned char *p=(const unsigned char*)&m;uint32_t h=2166136261u;
 for(unsigned i=0;i<sizeof(m);++i)h=(h^p[i])*16777619u;
 return h;
}
static void refresh(void)
{
 const uint32_t before=model_hash();m.online=settings_wifi_state()==WIFI_SET_CONNECTED;m.ble_enabled=settings_bt_enabled();
 struct tm time;
 if(mars_clock_get_local_time(&time)){snprintf(m.time,sizeof(m.time),"%02d:%02d",time.tm_hour,time.tm_min);static const char*week[]={"日","一","二","三","四","五","六"};snprintf(m.date,sizeof(m.date),"%02d 月 %02d 日  星期%s",time.tm_mon+1,time.tm_mday,week[time.tm_wday%7]);}
 else{copy(m.time,sizeof(m.time),"--:--");copy(m.date,sizeof(m.date),m.online?"正在同步时间":"连接 Wi-Fi 后同步时间");}
 copy(m.version,sizeof(m.version),esp_app_get_description()->version);
 if(m.page==ROUND_APPS){m.count=0;for(int i=0;i<MARS_APP_SLOTS&&m.count<16;++i){const char*id=mars_dynamic_bridge_package_name(i);if(!id)continue;round_item_t*t=m.items+m.count++;memset(t,0,sizeof(*t));copy(t->name,sizeof(t->name),mars_dynamic_bridge_display_name(i));t->key=i;t->icon=package_icon(id);}snprintf(m.status,sizeof(m.status),"%d 个已安装应用",m.count);}
 if(m.page==ROUND_SETTINGS)copy(m.status,sizeof(m.status),wifi_status());
 if(m.page==ROUND_WIFI){m.count=0;copy(m.status,sizeof(m.status),settings_wifi_scan_done()?wifi_status():"正在扫描...");if(settings_wifi_state()!=WIFI_SET_NONE&&settings_wifi_ssid()[0]){round_item_t*t=m.items+m.count++;memset(t,0,sizeof(*t));copy(t->name,sizeof(t->name),settings_wifi_ssid());copy(t->detail,sizeof(t->detail),wifi_status());t->key=-1;}
  for(int i=0;i<settings_wifi_scan_count()&&m.count<16;++i){const settings_wifi_ap_t*ap=settings_wifi_scan_ap(i);if(!ap||!ap->ssid[0])continue;round_item_t*t=m.items+m.count++;memset(t,0,sizeof(*t));copy(t->name,sizeof(t->name),ap->ssid);snprintf(t->detail,sizeof(t->detail),"%d dBm  %s",ap->rssi,ap->secure?"加密网络":"开放网络");t->key=i;t->secure=ap->secure;}}
 if(m.page==ROUND_BLE){
  m.count=0;settings_bt_state_t state=settings_bt_state();
  copy(m.status,sizeof(m.status),!m.ble_enabled?"蓝牙已关闭":!settings_bt_scan_done()?"正在扫描...":state==BT_SET_CONNECTED?"已连接":state==BT_SET_CONNECTING?"连接中":"未发现设备");
  if(state==BT_SET_CONNECTED||state==BT_SET_CONNECTING){
   round_item_t*t=m.items+m.count++;memset(t,0,sizeof(*t));
   copy(t->name,sizeof(t->name),settings_bt_connected_name());
   copy(t->detail,sizeof(t->detail),state==BT_SET_CONNECTED?"已连接 / 点击断开":"正在连接...");
   t->key=state==BT_SET_CONNECTED?-1:-2;
  }
  for(int i=0;i<settings_bt_scan_count()&&m.count<16;++i){
   const settings_bt_dev_t*d=settings_bt_scan_dev(i);if(!d)continue;
   round_item_t*t=m.items+m.count++;memset(t,0,sizeof(*t));
   if(d->name[0])copy(t->name,sizeof(t->name),d->name);
   else snprintf(t->name,sizeof(t->name),"%02X:%02X:%02X",d->addr[3],d->addr[4],d->addr[5]);
   snprintf(t->detail,sizeof(t->detail),"%d dBm",d->rssi);t->key=i;
  }
 }
 if(m.page==ROUND_NETWORK)copy(m.status,sizeof(m.status),wifi_status());
 int per=m.page==ROUND_APPS?4:3,max=m.count?(m.count-1)/per:0;if(m.index>max)m.index=max;
 if(before!=model_hash())dirty=true;
}
/* The painter fully clears/redraws in CPU byte order. Convert once per page,
 * then keep both cached pages in panel byte order throughout the transition. */
static void wire_image(uint16_t *pixels)
{ for(int i=0;i<412*412;++i)pixels[i]=__builtin_bswap16(pixels[i]); }
static void draw_page(void)
{ round_view_draw(&canvas,&m);wire_image(canvas.pixels); }
static void present(int offset)
{
 /* Compose straight into the BSP shadow, then DMA through its existing strip.
  * No extra wire buffer or per-pixel source-selection branches. */
 ESP_ERROR_CHECK(mars_board_present_slide(canvas.pixels,previous,offset));
}
static void begin_animation(int direction);
static void show(int page,int direction)
{
 if(animating)return;
 int from=m.page;int64_t prepare_start=esp_timer_get_time();
 if(direction)memcpy(previous,canvas.pixels,412*412*2);
 if(m.page==ROUND_KEYBOARD&&page!=ROUND_KEYBOARD)memset(m.password,0,sizeof(m.password));
 m.page=page;m.index=0;dirty=true;
 refresh();draw_page();dirty=false;
 ESP_LOGI(TAG,"page prepare from=%d to=%d elapsed_us=%lld",from,page,(long long)(esp_timer_get_time()-prepare_start));
 if(direction)begin_animation(direction);
 else present(0);
}
static void back(void)
{
 if(m.page==ROUND_KEYBOARD){memset(m.password,0,sizeof(m.password));show(ROUND_WIFI,1);}
 else if(m.page==ROUND_NETWORK)show(ROUND_WIFI,1);
 else if(m.page==ROUND_WIFI||m.page==ROUND_BLE||m.page==ROUND_ABOUT)show(ROUND_SETTINGS,1);
 else if(m.page!=ROUND_HOME)show(ROUND_HOME,m.page==ROUND_APPS?-1:1);
}
static void activate(round_hit_t hit)
{
 if(hit.action==RH_BACK){back();return;}
 if(hit.action==RH_APP){snprintf(pending_action,sizeof(pending_action),"app.dyn.%d.open",m.items[hit.value].key);return;}
 if(hit.action==RH_SETTING){int page=hit.value==0?ROUND_WIFI:hit.value==1?ROUND_BLE:ROUND_ABOUT;if(page==ROUND_WIFI)settings_wifi_scan_start();show(page,-1);return;}
 if(hit.action==RH_SCAN){if(m.page==ROUND_WIFI)settings_wifi_scan_start();else if(m.ble_enabled)settings_bt_scan_start();m.index=0;refresh();dirty=true;return;}
 if(hit.action==RH_TOGGLE){settings_bt_set_enabled(!m.ble_enabled);if(!m.ble_enabled)settings_bt_scan_start();refresh();dirty=true;return;}
 if(hit.action==RH_ITEM){round_item_t item=m.items[hit.value];if(m.page==ROUND_BLE){if(item.key==-1)settings_bt_disconnect();else if(item.key>=0)settings_bt_connect(item.key);refresh();dirty=true;return;}copy(m.ssid,sizeof(m.ssid),item.name);if(item.key==-1){show(ROUND_NETWORK,-1);return;}if(!item.secure){settings_wifi_connect(m.ssid,"");dirty=true;return;}memset(m.password,0,sizeof(m.password));m.mode=WIFI_KEYS_LETTERS;m.key_page=0;m.upper=false;show(ROUND_KEYBOARD,-1);return;}
 if(hit.action==RH_PREV||hit.action==RH_NEXT){int per=m.page==ROUND_APPS?4:3,pages=(m.count+per-1)/per;if(pages>0)m.index=(m.index+pages+(hit.action==RH_NEXT?1:-1))%pages;dirty=true;return;}
 if(hit.action==RH_KEYMODE){switch(hit.value){case 0:m.upper=!m.upper;m.mode=WIFI_KEYS_LETTERS;break;case 1:m.mode=WIFI_KEYS_NUMBERS;m.key_page=0;break;case 2:m.mode=WIFI_KEYS_SYMBOLS;m.key_page=0;break;case 3:if(m.mode==WIFI_KEYS_LETTERS)m.key_page=(m.key_page+1)%2;else{m.mode=WIFI_KEYS_LETTERS;m.key_page=0;}break;case 4:{size_t n=strlen(m.password);if(n)m.password[n-1]=0;}break;}dirty=true;return;}
 if(hit.action==RH_CHAR){wifi_key_t key=wifi_keys_key(m.mode,m.key_page,m.upper,hit.value);if(key.kind==WIFI_KEY_NEXT_PAGE)m.key_page=(m.key_page+1)%3;else if(key.kind==WIFI_KEY_CHARACTER){size_t n=strlen(m.password);if(n<63){m.password[n]=key.character;m.password[n+1]=0;}}dirty=true;return;}
 if(hit.action==RH_CONNECT){settings_wifi_connect(m.ssid,m.password);memset(m.password,0,sizeof(m.password));show(ROUND_WIFI,1);return;}
 if(hit.action==RH_DISCONNECT||hit.action==RH_FORGET){if(hit.action==RH_FORGET)settings_wifi_forget();else settings_wifi_disconnect();show(ROUND_WIFI,1);return;}
}
static int app_ui_begin(int width,int height)
{
 if(active||width<1||height<1||width>512||height>512)return -1;
 app_width=width;app_height=height;native_app=true;mc_clear(&canvas,0);
 ESP_LOGI(TAG,"native app UI v1 logical=%dx%d surface=412x412",width,height);return 0;
}
static void app_ui_clear(uint16_t color){mc_clear(&canvas,color);}
static void app_ui_rect(int x,int y,int w,int h,uint16_t color){round_app_rect(&canvas,app_width,app_height,x,y,w,h,color);}
static void app_ui_text(int x,int y,const char*s,int scale,uint16_t color){round_app_text(&canvas,app_width,app_height,x,y,s,scale,color);}
static int app_ui_width(const char *s,int scale)
{return round_app_text_width(s,scale)*(app_width>app_height?app_width:app_height)/320;}
static void app_ui_bitmap(int x,int y,int w,int h,int scale,const uint16_t*p){round_app_bitmap(&canvas,app_width,app_height,x,y,w,h,scale,p);}
static void app_ui_legacy(void){round_app_bitmap(&canvas,app_width,app_height,0,0,320,240,1,gfx_frame_pixels());}
static void app_ui_present(void)
{
 if(gfx_first_frame_barrier())return;
 ESP_ERROR_CHECK(mars_board_present_cpu_frame(canvas.pixels));
}
static const mars_native_ui_v1_t native_ui={1,412,412,1,app_ui_begin,app_ui_clear,
 app_ui_rect,app_ui_text,app_ui_width,app_ui_bitmap,app_ui_legacy,app_ui_present};
bool round_ui_app_native(void){return native_app;}
bool round_ui_app_map_touch(uint16_t x,uint16_t y,uint16_t *vx,uint16_t *vy)
{return native_app&&round_app_unmap(x,y,app_width,app_height,vx,vy);}
bool round_ui_init(void)
{
 if(canvas.pixels)return true;
 if(MARS_BOARD_DISPLAY_WIDTH!=412||MARS_BOARD_DISPLAY_HEIGHT!=412)return false;
 canvas=(mc_canvas_t){heap_caps_malloc(412*412*2,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT),412,412};
 previous=heap_caps_malloc(412*412*2,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
 if(!canvas.pixels||!previous){free(canvas.pixels);free(previous);canvas.pixels=previous=NULL;ESP_LOGE(TAG,"native canvas unavailable; retaining compatible UI");return false;}
 mars_gesture_init(&gestures,(mars_gesture_config_t){412,412,44,28,20,600,true});
 mars_native_ui_register(&native_ui);
 active=true;dirty=true;m.page=ROUND_HOME;
 ESP_LOGI(TAG,"native 412x412 shell ready; MARSSNOW centered; legacy apps remain 320x240");return true;
}
bool round_ui_active(void){return active;}
bool round_ui_busy(void){return animating;}
static void animate_frame(int64_t now);
static void begin_animation(int direction)
{
 animation_sign=direction;animation_start=esp_timer_get_time()/1000;
 animation_duration=mars_ui_motion().transition_ms;animation_frames=0;
 animation_draw_us=0;animation_max_us=0;animating=true;wait_up=true;mars_gesture_barrier(&gestures);
}
/* App entry/exit lasts one bounded transition on the existing display owner.
 * The async app stays frozen until this returns; no rendering from callbacks. */
static void app_animation(int direction)
{
 begin_animation(direction);
 int64_t deadline=esp_timer_get_time();
 while(animating){
  animate_frame(esp_timer_get_time()/1000);
  if(!animating)break;
  deadline+=1000000/mars_ui_motion().fps;
  int64_t remain=(deadline-esp_timer_get_time())/1000;
  TickType_t ticks=remain>0?pdMS_TO_TICKS(remain):0;
  vTaskDelay(ticks?ticks:1);
 }
}
void round_ui_suspend(void)
{
 native_app=false;active=false;animating=false;down=false;wait_up=true;mars_gesture_barrier(&gestures);
 memset(m.password,0,sizeof(m.password));
 memcpy(previous,canvas.pixels,412*412*2);
}
void round_ui_app_enter(void)
{
 if(!canvas.pixels)return;
 if(!native_app){
  mc_clear(&canvas,0x0883);
  gfx_copy_frame(canvas.pixels+MARS_BOARD_UI_Y*412+MARS_BOARD_UI_X,412);
 }
 wire_image(canvas.pixels);app_snapshot=true;app_animation(-1);
 if(native_app)wire_image(canvas.pixels);
}
void round_ui_app_fallback(void)
{
 /* No safe first frame: allow the app to present normally after clearing
  * native margins. Exit can still snapshot after synchronous app close. */
 app_snapshot=true;ESP_ERROR_CHECK(mars_board_fill_rect(0,0,412,412,0x0883));
}
void round_ui_resume(void)
{
 if(!canvas.pixels)return;
 if(app_snapshot){
  if(native_app)memcpy(previous,canvas.pixels,412*412*2);
  else{
   for(int i=0;i<412*412;++i)previous[i]=0x0883;
   gfx_copy_frame(previous+MARS_BOARD_UI_Y*412+MARS_BOARD_UI_X,412);
  }
  wire_image(previous);
 }
 native_app=false;active=true;wait_up=true;mars_gesture_barrier(&gestures);down=false;dirty=true;
 refresh();draw_page();
 if(app_snapshot){app_snapshot=false;app_animation(1);}else present(0);
 dirty=false;
}
const char *round_ui_take_action(void){if(!pending_action[0])return NULL;copy(taken_action,sizeof(taken_action),pending_action);pending_action[0]=0;return taken_action;}
bool round_ui_action(const char*action)
{
 ESP_LOGI(TAG,"action state page=%d active=%d animation=%d drag=%d down=%d wait=%d",m.page,active,animating,0,down,wait_up);
 if(!active||animating)return false;
 if(!strcmp(action,"app.back")){back();return true;}
 if(!strcmp(action,"app.apps.close")&&m.page==ROUND_APPS){back();return true;}
 if(!strcmp(action,"app.apps.open")){show(ROUND_APPS,m.page==ROUND_HOME?1:0);return true;}
 if(!strcmp(action,"app.system.open")){show(ROUND_SETTINGS,m.page==ROUND_HOME?-1:0);return true;}
 if(!strcmp(action,"app.about.open")){show(ROUND_ABOUT,-1);return true;}
 return false;
}
static void animate_frame(int64_t now)
{
  float t=animation_duration?(now-animation_start)/(float)animation_duration:1;
  if(t<0)t=0;
  bool finished=t>=1;
  if(finished)t=1;
  float u=1-t,e=1-u*u*u; /* Fast start, gentle landing. */
  int offset=finished?0:(int)(animation_sign*412*e);
  if(!finished&&!offset)offset=animation_sign;
  int64_t started=esp_timer_get_time();present(offset);
  uint32_t draw_us=(uint32_t)(esp_timer_get_time()-started);
  animation_draw_us+=draw_us;++animation_frames;
  if(draw_us>animation_max_us)animation_max_us=draw_us;
  mars_ui_motion_sample(draw_us);
  if(finished){
   animating=false;dirty=false;
   ESP_LOGI(TAG,"transition frames=%u elapsed_ms=%lld draw_avg_us=%lu draw_max_us=%lu",
    animation_frames,(long long)(esp_timer_get_time()/1000-animation_start),
    (unsigned long)(animation_draw_us/animation_frames),(unsigned long)animation_max_us);
  }
}
void round_ui_poll(int64_t now)
{
 if(!active)return;
 /* No synchronous I2C transactions in the animation hot path. Input is
  * drained after completion; holding a finger never clicks the arriving page. */
 if(animating){animate_frame(now);return;}
 mars_touch_sample_t sample=mars_board_touch_sample();
 mars_gesture_event_t e=mars_gesture_update(&gestures,sample,now);
 wait_up=gestures.blocked;down=gestures.held;
 if(e.kind!=MARS_GESTURE_NONE&&e.kind!=MARS_GESTURE_MOVE&&m.page!=ROUND_KEYBOARD)
  ESP_LOGI(TAG,"gesture=%d page=%d dx=%d dy=%d",e.kind,m.page,e.dx,e.dy);
 if(e.kind==MARS_GESTURE_EDGE_BACK){
  if(m.page==ROUND_HOME){int page=round_view_swipe(m.page,e.dx>0?80:-80,0);if(page!=m.page)show(page,e.dx>0?1:-1);}
  else back();
  return;
 }
 if(e.kind==MARS_GESTURE_SWIPE_LEFT||e.kind==MARS_GESTURE_SWIPE_RIGHT){
  int page=round_view_swipe(m.page,e.dx>0?80:-80,0);
  if(page!=m.page)show(page,e.dx>0?1:-1);
  return;
 }
 if(e.kind==MARS_GESTURE_SWIPE_UP||e.kind==MARS_GESTURE_SWIPE_DOWN){
  if(m.page==ROUND_APPS||m.page==ROUND_WIFI||m.page==ROUND_BLE)
   activate((round_hit_t){e.kind==MARS_GESTURE_SWIPE_UP?RH_NEXT:RH_PREV,0});
  return;
 }
 if(e.kind==MARS_GESTURE_TAP)activate(round_view_hit(&m,e.start_x,e.start_y));
 mars_input_action_t key=mars_input_update(&input,mars_board_key_read(),now);
 if(key==MARS_INPUT_BACK)back();else if(key==MARS_INPUT_NEXT){if(m.page==ROUND_HOME)show(ROUND_APPS,1);else if(m.page==ROUND_APPS)activate((round_hit_t){RH_NEXT,0});else if(m.page==ROUND_SETTINGS)show(ROUND_HOME,1);}else if(key==MARS_INPUT_SELECT){if(m.page==ROUND_HOME)show(ROUND_SETTINGS,-1);else back();}
 if(animating)return;
 if(now-last_refresh>=300){last_refresh=now;refresh();}
 if(dirty){draw_page();present(0);dirty=false;}
}
