#include "mars_board.h"
#include "box3_board.h"
#include "driver/i2c_master.h"

static bool codec_present;
static bool initialized;

esp_err_t mars_board_init(void)
{
    if (initialized) return ESP_OK;
    esp_err_t result = box3_board_init();
    i2c_master_bus_handle_t bus = NULL;
    if (result == ESP_OK && box3_i2c_bus_get(&bus) == ESP_OK)
        codec_present = i2c_master_probe(bus, MARS_BOARD_CODEC_ADDRESS, 100) == ESP_OK;
    initialized = result == ESP_OK;
    return result;
}

bool mars_board_codec_present(void) { return codec_present; }
esp_err_t mars_board_i2c_bus_get(i2c_master_bus_handle_t *bus) { return box3_i2c_bus_get(bus); }
bool mars_board_touch_available(void) { return box3_touch_available(); }
int mars_board_key_read(void) { return box3_key_read(); }
bool mars_board_touch_read(uint16_t *x, uint16_t *y) { return box3_touch_read(x, y); }
esp_err_t mars_board_fill_rect(int x, int y, int w, int h, uint16_t color)
{ return box3_display_fill_rect(x, y, w, h, color); }
esp_err_t mars_board_blit(int x, int y, int w, int h, const uint16_t *pixels)
{ return box3_display_blit(x, y, w, h, pixels); }
esp_err_t mars_board_battery_read(int *percent, int *millivolts)
{
    (void)percent;
    (void)millivolts;
    return ESP_ERR_NOT_SUPPORTED;
}
