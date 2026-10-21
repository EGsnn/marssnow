#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "mars_board_config.h"

#define BOX3_LCD_WIDTH  MARS_BOARD_UI_WIDTH
#define BOX3_LCD_HEIGHT MARS_BOARD_UI_HEIGHT

/* 兼容接口：应用坐标为 UI 窗口，板载 I2C 总线由 BSP 统一持有。 */
struct i2c_master_bus_t;
typedef struct i2c_master_bus_t *i2c_master_bus_handle_t;

esp_err_t box3_board_init(void);
esp_err_t box3_i2c_bus_get(i2c_master_bus_handle_t *bus);
esp_err_t box3_display_fill_rect(int x, int y, int width, int height, uint16_t color);
esp_err_t box3_display_blit(int x, int y, int width, int height, const uint16_t *pixels);
bool box3_touch_read(uint16_t *x, uint16_t *y);
bool box3_touch_available(void);
int box3_key_read(void);
