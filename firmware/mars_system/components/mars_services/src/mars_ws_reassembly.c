#include "mars_ws_reassembly.h"
#include <string.h>
int mars_ws_reassembly_feed(mars_ws_reassembly_t *r, int op, bool fin,
                            size_t total, size_t offset, const void *data, size_t len)
{
    if(!r || !r->data || offset>total || len>total-offset || (len && !data))return -71;
    if(offset==0) {
        if(r->frame_active)return -71;
        if(op==1 || op==2) {
            if(r->opcode)return -71;
            r->opcode=op;r->length=0;
        } else if(op!=0 || !r->opcode)return -71;
        r->frame_length=total;r->frame_offset=0;r->frame_active=true;
    }
    if(!r->frame_active || offset!=r->frame_offset || total!=r->frame_length)return -71;
    if(total>r->capacity || r->length>r->capacity || len>r->capacity-r->length)return -90;
    if(len)memcpy(r->data+r->length,data,len);
    r->length+=len;r->frame_offset+=len;
    if(r->frame_offset==total) {
        r->frame_active=false;
        if(fin)return 1;
    }
    return 0;
}
