#pragma once
#include "mars_canvas.h"
#include "settings_keyboard.h"
#include <stdbool.h>
enum { ROUND_HOME,ROUND_APPS,ROUND_SETTINGS,ROUND_WIFI,ROUND_KEYBOARD,ROUND_BLE,ROUND_ABOUT,ROUND_NETWORK };
enum { RH_NONE,RH_BACK,RH_APP,RH_SETTING,RH_ITEM,RH_PREV,RH_NEXT,RH_SCAN,RH_TOGGLE,RH_KEYMODE,RH_CHAR,RH_CONNECT,RH_DISCONNECT,RH_FORGET };
typedef struct { char name[64],detail[64];int key,icon;bool secure; } round_item_t;
typedef struct {
 int page,index,count,focus;
 round_item_t items[16];
 char time[8],date[80],status[80],ssid[33],version[48];
 char password[64];
 bool online,ble_enabled,upper;
 wifi_keys_mode_t mode;
 unsigned key_page;
} round_model_t;
typedef struct {int action,value;} round_hit_t;
void round_view_draw(mc_canvas_t *,const round_model_t *);
round_hit_t round_view_hit(const round_model_t *,int x,int y);
/* Finger direction, not content order. Details/keyboards have no root swipe. */
int round_view_swipe(int page,int dx,int dy);
