#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
/* Decode only the first contact. Reject malformed packet lengths and releases. */
static inline bool spd2010_first_contact(const uint8_t *packet,size_t length,
                                       unsigned width,unsigned height,uint16_t *x,uint16_t *y)
{
    if (!packet || !x || !y || length<10 || length>64 || (length-4)%6
        || packet[4]>10 || packet[8]==0) return false;
    unsigned px=((packet[7]&0xf0)<<4)|packet[5];
    unsigned py=((packet[7]&0x0f)<<8)|packet[6];
    if (px>=width || py>=height) return false;
    *x=px; *y=py; return true;
}

/* Decode report validity separately from pressure. A valid zero-pressure
 * report is a release with coordinates, not malformed/no data. */
static inline bool spd2010_contact_report(const uint8_t *p,size_t n,unsigned width,unsigned height,
                                         uint16_t *x,uint16_t *y,bool *pressed)
{
 if(!p||!x||!y||!pressed||n<10||n>64||(n-4)%6||p[4]>10)return false;
 unsigned px=((p[7]&0xf0)<<4)|p[5],py=((p[7]&15)<<8)|p[6];
 if(px>=width||py>=height)return false;
 *x=px;*y=py;*pressed=p[8]!=0;return true;
}
