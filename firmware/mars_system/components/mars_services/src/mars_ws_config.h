#pragma once
#include <stdint.h>

typedef struct {
    char url[512];
    char token[1024];
    char uuid[64];
    uint32_t version;
} mars_ws_config_t;

/* Startup only, on the system's internal-RAM stack. Immutable until reboot.
 * Dynamic apps have PSRAM stacks: even read-only NVS can disable that cache. */
void mars_ws_config_init(void);
int mars_ws_config_status(void);
const mars_ws_config_t *mars_ws_config_current(void);
