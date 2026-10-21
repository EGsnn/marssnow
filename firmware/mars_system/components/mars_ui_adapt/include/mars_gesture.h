#pragma once
#include "mars_touch.h"
#include <stdbool.h>
#include <stdint.h>
typedef enum {MARS_GESTURE_NONE,MARS_GESTURE_PRESS,MARS_GESTURE_MOVE,
 MARS_GESTURE_RELEASE,MARS_GESTURE_TAP,MARS_GESTURE_LONG_PRESS,
 MARS_GESTURE_SWIPE_LEFT,MARS_GESTURE_SWIPE_RIGHT,MARS_GESTURE_SWIPE_UP,
 MARS_GESTURE_SWIPE_DOWN,MARS_GESTURE_EDGE_BACK,MARS_GESTURE_CANCEL} mars_gesture_kind_t;
typedef struct {int width,height,edge,swipe,tap_slop,long_ms;bool round;} mars_gesture_config_t;
typedef struct {mars_gesture_kind_t kind;int x,y,start_x,start_y,dx,dy;int64_t time_ms;} mars_gesture_event_t;
typedef struct {
 mars_gesture_config_t config;
 bool held,consumed,long_sent,edge_start,blocked;
 int start_x,start_y,x,y,max_distance;
 int64_t pressed_at,quiet_since;
} mars_gesture_t;
void mars_gesture_init(mars_gesture_t *,mars_gesture_config_t);
/* Barrier keeps held contacts consumed across page/app ownership changes. */
void mars_gesture_barrier(mars_gesture_t *);
mars_gesture_event_t mars_gesture_update(mars_gesture_t *,mars_touch_sample_t,int64_t now_ms);
