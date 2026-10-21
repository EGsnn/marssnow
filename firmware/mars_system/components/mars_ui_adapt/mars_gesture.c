#include "mars_gesture.h"
#include <stdlib.h>
#include <string.h>
static int root(int value){int low=0,high=1024;while(low+1<high){int mid=(low+high)/2;if(mid*mid<=value)low=mid;else high=mid;}return low;}
static bool edge(mars_gesture_t *s,int x,int y)
{
 int left=0,right=s->config.width-1;
 if(s->config.round){int r=s->config.width/2,dy=y-s->config.height/2;int q=r*r-dy*dy;if(q<0)return false;int span=root(q);left=r-span;right=r+span;}
 return x-left<=s->config.edge||right-x<=s->config.edge;
}
void mars_gesture_init(mars_gesture_t *s,mars_gesture_config_t config)
{memset(s,0,sizeof(*s));s->config=config;}
void mars_gesture_barrier(mars_gesture_t *s)
{s->held=false;s->blocked=true;s->consumed=true;s->quiet_since=0;}
static mars_gesture_event_t event(mars_gesture_t *s,mars_gesture_kind_t kind,int64_t now)
{return(mars_gesture_event_t){kind,s->x,s->y,s->start_x,s->start_y,s->x-s->start_x,s->y-s->start_y,now};}
mars_gesture_event_t mars_gesture_update(mars_gesture_t *s,mars_touch_sample_t p,int64_t now)
{
 if(s->blocked){
  if(p.down)s->quiet_since=0;
  else if(!s->quiet_since)s->quiet_since=now;
  else if(now-s->quiet_since>=60){s->blocked=false;s->consumed=false;s->quiet_since=0;}
  return event(s,MARS_GESTURE_NONE,now);
 }
 if(p.phase==MARS_TOUCH_CANCEL){mars_gesture_barrier(s);return event(s,MARS_GESTURE_CANCEL,now);}
 if(p.down&&(p.x>=s->config.width||p.y>=s->config.height||p.fingers>1)){
  mars_gesture_barrier(s);return event(s,MARS_GESTURE_CANCEL,now);
 }
 if(p.phase==MARS_TOUCH_POINT&&p.down&&!s->held){
  s->held=true;s->consumed=false;s->long_sent=false;s->max_distance=0;
  s->start_x=s->x=p.x;s->start_y=s->y=p.y;s->pressed_at=now;s->edge_start=edge(s,p.x,p.y);
  return event(s,MARS_GESTURE_PRESS,now);
 }
 if(!s->held)return event(s,MARS_GESTURE_NONE,now);
 if(p.phase==MARS_TOUCH_POINT||p.phase==MARS_TOUCH_UP){
  /* Some controllers send zero coordinates for release. The round panel's
   * (0,0) is outside the glass; retain the last genuine point in that case. */
  if(p.x||p.y||!s->config.round){s->x=p.x;s->y=p.y;}
 }
 int dx=s->x-s->start_x,dy=s->y-s->start_y;
 int distance=abs(dx)>abs(dy)?abs(dx):abs(dy);
 if(distance>s->max_distance)s->max_distance=distance;
 if(!s->consumed&&abs(dx)>=s->config.swipe&&abs(dx)*100>abs(dy)*135){
  s->consumed=true;if(p.phase==MARS_TOUCH_UP)s->held=false;
  bool inward=s->start_x<s->config.width/2?dx>0:dx<0;
  return event(s,s->edge_start&&inward?MARS_GESTURE_EDGE_BACK:dx>0?MARS_GESTURE_SWIPE_RIGHT:MARS_GESTURE_SWIPE_LEFT,now);
 }
 if(!s->consumed&&abs(dy)>=s->config.swipe&&abs(dy)*100>abs(dx)*135){
  s->consumed=true;if(p.phase==MARS_TOUCH_UP)s->held=false;return event(s,dy>0?MARS_GESTURE_SWIPE_DOWN:MARS_GESTURE_SWIPE_UP,now);
 }
 if(!s->consumed&&!s->long_sent&&p.down&&s->max_distance<=s->config.tap_slop&&now-s->pressed_at>=s->config.long_ms){
  s->long_sent=true;s->consumed=true;return event(s,MARS_GESTURE_LONG_PRESS,now);
 }
 if(p.phase==MARS_TOUCH_UP){
  s->held=false;bool tap=!s->consumed&&s->max_distance<=s->config.tap_slop;
  return event(s,tap?MARS_GESTURE_TAP:MARS_GESTURE_RELEASE,now);
 }
 return event(s,p.phase==MARS_TOUCH_POINT?MARS_GESTURE_MOVE:MARS_GESTURE_NONE,now);
}
