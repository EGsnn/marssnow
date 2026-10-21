#pragma once

#include <stdint.h>

/* Returns a read-only, flash-mapped 320x240 RGB565 wallpaper, or NULL. */
const uint16_t *mars_assets_landscape(void);
