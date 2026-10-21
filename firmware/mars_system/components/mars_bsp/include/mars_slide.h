#pragma once
#include <stdint.h>
#include <string.h>
/* Cached pages already have the panel's byte order. Animation is two memcpy
 * spans per row; zero is the completed page, offset in [-width,width]. */
static inline void mars_slide_row(uint16_t *dst,const uint16_t *next,const uint16_t *old,int width,int offset)
{
 if(!offset){memcpy(dst,next,width*sizeof(uint16_t));return;}
 int split=offset>0?offset:width+offset;
 const uint16_t *left=offset>0?next+width-offset:old-offset;
 const uint16_t *right=offset>0?old:next;
 memcpy(dst,left,split*sizeof(uint16_t));
 memcpy(dst+split,right,(width-split)*sizeof(uint16_t));
}
