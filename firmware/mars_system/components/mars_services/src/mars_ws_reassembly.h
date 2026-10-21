#pragma once
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
typedef struct {
    uint8_t *data;
    size_t capacity, length, frame_offset, frame_length;
    int opcode;
    bool frame_active;
} mars_ws_reassembly_t;
/* Returns 1 for complete message, 0 for partial, negative for malformed/oversize. */
int mars_ws_reassembly_feed(mars_ws_reassembly_t *r, int opcode, bool fin,
                            size_t frame_length, size_t offset,
                            const void *data, size_t length);
