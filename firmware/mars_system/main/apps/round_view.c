#include "round_view.h"
#include "mars_board_config.h"
#include <math.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#define RGB(r,g,b) (uint16_t)((((r)&248)<<8)|(((g)&252)<<3)|((b)>>3))
enum { BG=MARS_UI_TOKENS_BACKGROUND,TEXT=MARS_UI_TOKENS_TEXT,MINT=MARS_UI_TOKENS_ACCENT,MUTED=RGB(143,172,155),PANEL=MARS_UI_TOKENS_SURFACE };
static void text(mc_canvas_t*c,int x,int y,const char*s,int size,uint16_t color){mc_text(c,x,y,s,size,color);}
static void center(mc_canvas_t*c,int y,const char*s,int size,uint16_t color){mc_text_center(c,206,y,s,size,color);}
static void pill(mc_canvas_t*c,int x,int y,int w,int h,const char*s,bool active){mc_round_rect(c,x,y,w,h,h/3,active?MINT:PANEL);mc_text_center(c,x+w/2,y+h/2+6,s,16,active?BG:TEXT);}
static void title(mc_canvas_t*c,const char*s){center(c,83,s,24,TEXT);}
static void clipped_text(mc_canvas_t*c,int x,int base,const char*s,int size,int width,uint16_t color)
{
 char b[100]={0};int at=0;while(*s&&at<90){int n=((unsigned char)*s<128)?1:((unsigned char)*s<224?2:3);if((int)strlen(s)<n)break;memcpy(b+at,s,n);b[at+n]=0;if(mc_text_width(b,size)>width-16){b[at]=0;strcat(b,"..");break;}at+=n;s+=n;}text(c,x,base,b,size,color);
}
static void pages(mc_canvas_t*c,const round_model_t*m,int per)
{
 if(m->count>per){mc_icon(c,MC_BACK,121,351,22,MUTED);mc_icon(c,MC_NEXT,269,351,22,MUTED);}
}
static void orbit(mc_canvas_t*c)
{
 mc_circle(c,206,206,175,1,RGB(41,71,55));
 for(int i=0;i<60;++i){float a=(i*6-90)*.0174532925f;float r=i%5?190:187;mc_line(c,206+cosf(a)*r,206+sinf(a)*r,206+cosf(a)*194,206+sinf(a)*194,i%5?1:1.5f,i%5?RGB(49,72,59):RGB(88,119,97));}
 for(int i=0;i<40;++i){float a=(208+i*42.f/40)*.0174532925f,b=(208+(i+1)*42.f/40)*.0174532925f;mc_line(c,206+cosf(a)*175,206+sinf(a)*175,206+cosf(b)*175,206+sinf(b)*175,2.5f,MINT);}
 mc_circle(c,51,125,3,0,MINT);mc_circle(c,353,301,2.5f,0,MUTED);
}
void round_view_draw(mc_canvas_t*c,const round_model_t*m)
{
 mc_clear(c,BG);
 if(m->page==ROUND_HOME){orbit(c);mc_arc_brand(c,206,206,130,"MARSSNOW",15,m->online?MINT:MUTED,true);center(c,231,m->time,90,TEXT);center(c,278,m->date,16,MUTED);return;}
 if(m->page==ROUND_APPS){title(c,"应用");center(c,110,m->status,12,MUTED);for(int i=0;i<4;++i){int at=m->index*4+i;if(at>=m->count)break;int x=139+(i%2)*134,y=131+(i/2)*104;const round_item_t*item=m->items+at;mc_round_rect(c,x-27,y,54,54,18,PANEL);mc_icon(c,item->icon,x-14,y+13,28,MINT);int w=(int)mc_text_width(item->name,16);if(w<=116)text(c,x-w/2,y+81,item->name,16,TEXT);else clipped_text(c,x-54,y+81,item->name,16,108,TEXT);}if(!m->count)center(c,212,"尚未安装应用",16,MUTED);pages(c,m,4);return;}
 if(m->page==ROUND_SETTINGS){title(c,"设置");const char*names[]={"Wi-Fi","蓝牙","关于"};const char*sub[]={m->online?"已连接":"点击配置网络","BLE 设备","火星系统"};int icons[]={MC_WIFI,MC_BLE,MC_INFO};for(int i=0;i<3;++i){int y=119+i*72;mc_round_rect(c,66,y,280,60,20,PANEL);mc_icon(c,icons[i],84,y+18,24,MINT);text(c,122,y+27,names[i],18,TEXT);text(c,122,y+48,sub[i],12,MUTED);mc_icon(c,MC_NEXT,318,y+20,18,MUTED);}return;}
 if(m->page==ROUND_WIFI||m->page==ROUND_BLE){bool ble=m->page==ROUND_BLE;title(c,ble?"蓝牙":"Wi-Fi");
  if(ble){pill(c,84,97,100,32,m->ble_enabled?"关闭 BLE":"开启 BLE",m->ble_enabled);pill(c,228,97,100,32,"扫描",false);}else{clipped_text(c,91,111,m->status,12,195,MUTED);pill(c,286,88,48,32,"扫描",false);}
  for(int i=0;i<3;++i){int at=m->index*3+i;if(at>=m->count)break;int y=139+i*61;const round_item_t*item=m->items+at;mc_round_rect(c,66,y,280,53,17,PANEL);clipped_text(c,84,y+22,item->name,16,220,TEXT);clipped_text(c,84,y+43,item->detail,12,220,MUTED);mc_icon(c,item->secure?MC_LOCK:MC_NEXT,318,y+18,16,MUTED);}
  if(!m->count){center(c,212,m->status,16,MUTED);center(c,244,ble?"仅支持低功耗蓝牙":"请稍候或重新扫描",12,MUTED);}pages(c,m,3);return;}
 if(m->page==ROUND_KEYBOARD){title(c,"输入密码");clipped_text(c,94,108,m->ssid,12,224,MUTED);mc_round_rect(c,80,116,252,38,13,PANEL);char mask[32];size_t n=strlen(m->password);if(n>20)n=20;memset(mask,'*',n);mask[n]=0;text(c,94,142,n?mask:"输入密码",n?20:16,n?TEXT:MUTED);
  const char*control[]={m->upper?"A/a":"a/A","123","#+=",m->mode==WIFI_KEYS_LETTERS?(m->key_page%2?"左半":"右半"):"ABC","删除"};for(int i=0;i<5;++i)pill(c,64+i*58,164,52,34,control[i],(i==1&&m->mode==WIFI_KEYS_NUMBERS)||(i==2&&m->mode==WIFI_KEYS_SYMBOLS));
  for(int i=0;i<15;++i){wifi_key_t k=wifi_keys_key(m->mode,m->key_page,m->upper,i);char b[2]={k.character,0};if(k.kind!=WIFI_KEY_NONE)pill(c,64+(i%5)*58,210+(i/5)*42,52,36,k.label?k.label:b,false);}pill(c,148,345,116,40,"连接",true);return;}
 if(m->page==ROUND_NETWORK){title(c,"网络");clipped_text(c,80,126,m->ssid,16,252,TEXT);center(c,163,m->status,16,MUTED);pill(c,84,191,244,50,"断开连接",false);pill(c,84,253,244,50,"忘记网络",false);return;}
 title(c,"关于");mc_circle(c,206,162,32,0,PANEL);mc_icon(c,MC_INFO,189,145,34,MINT);center(c,230,"MARSSNOW",20,TEXT);center(c,261,m->version,14,MUTED);center(c,291,"ESP32-S3 · 412 x 412",12,MUTED);
}
static bool box(int x,int y,int l,int t,int w,int h){return x>=l&&y>=t&&x<l+w&&y<t+h;}
round_hit_t round_view_hit(const round_model_t*m,int x,int y)
{
 round_hit_t none={RH_NONE,0};if(x<0||y<0||x>=412||y>=412)return none;
 if((x-206)*(x-206)+(y-206)*(y-206)>194*194)return none;
 if(m->page==ROUND_HOME)return none;
 if(m->page==ROUND_KEYBOARD){if(box(x,y,148,345,116,40))return(round_hit_t){RH_CONNECT,0};
  for(int i=0;i<5;++i)if(box(x,y,64+i*58,164,52,34))return(round_hit_t){RH_KEYMODE,i};
  for(int i=0;i<15;++i)if(box(x,y,64+(i%5)*58,210+(i/5)*42,52,36))return(round_hit_t){RH_CHAR,i};
  return none;}
 if(m->page==ROUND_SETTINGS){for(int i=0;i<3;++i)if(box(x,y,66,119+i*72,280,60))return(round_hit_t){RH_SETTING,i};}
 if(m->page==ROUND_APPS){for(int i=0;i<4;++i)if(m->index*4+i<m->count&&box(x,y,88+(i%2)*134,131+(i/2)*104,102,85))return(round_hit_t){RH_APP,m->index*4+i};}
 if(m->page==ROUND_WIFI||m->page==ROUND_BLE){bool ble=m->page==ROUND_BLE;if(box(x,y,ble?228:286,ble?97:88,ble?100:48,32))return(round_hit_t){RH_SCAN,0};if(ble&&box(x,y,84,97,100,32))return(round_hit_t){RH_TOGGLE,0};for(int i=0;i<3;++i)if(m->index*3+i<m->count&&box(x,y,66,139+i*61,280,53))return(round_hit_t){RH_ITEM,m->index*3+i};}
 int per=m->page==ROUND_APPS?4:3;if(m->count>per&&(m->page==ROUND_APPS||m->page==ROUND_WIFI||m->page==ROUND_BLE)){if(box(x,y,108,338,48,48))return(round_hit_t){RH_PREV,0};if(box(x,y,256,338,48,48))return(round_hit_t){RH_NEXT,0};}
 if(m->page==ROUND_NETWORK){if(box(x,y,84,191,244,50))return(round_hit_t){RH_DISCONNECT,0};if(box(x,y,84,253,244,50))return(round_hit_t){RH_FORGET,0};}return none;
}
int round_view_swipe(int page,int dx,int dy)
{
 if(abs(dx)<56||abs(dx)*100<=abs(dy)*135)return page;
 if(page==ROUND_HOME)return dx>0?ROUND_APPS:ROUND_SETTINGS;
 if((page==ROUND_APPS&&dx<0)||(page==ROUND_SETTINGS&&dx>0))return ROUND_HOME;
 return page;
}
