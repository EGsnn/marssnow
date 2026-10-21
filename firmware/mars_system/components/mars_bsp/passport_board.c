/* AI Passport reference mapping, from FoloToy BSP and the local XiaoZhi port.
 * No PSRAM, touch, full framebuffer, codec ownership or NFC reader is assumed. */
#include "mars_board.h"

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "driver/ledc.h"
#include "driver/spi_master.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_attr.h"
#include "esp_heap_caps.h"
#include <stdlib.h>
#include "esp_check.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

enum { TRANSFER_PIXELS=MARS_BOARD_DISPLAY_WIDTH*40 };
static const char *TAG = "passport_board";
static i2c_master_bus_handle_t bus;
static i2c_master_dev_handle_t gauge;
static esp_lcd_panel_handle_t panel;
static SemaphoreHandle_t color_done;
static adc_oneshot_unit_handle_t adc;
static adc_cali_handle_t calibration;
static bool codec_present;
static bool init_attempted;
static bool initialized;
/* Clear operations allocate a temporary DMA strip; UI owns its own reusable strip. */

static bool transfer_done(esp_lcd_panel_io_handle_t io,
                          esp_lcd_panel_io_event_data_t *event, void *ctx)
{
    (void)io;
    (void)event;
    BaseType_t wake = pdFALSE;
    xSemaphoreGiveFromISR((SemaphoreHandle_t)ctx, &wake);
    return wake == pdTRUE;
}

static esp_err_t init_i2c(void)
{
    const i2c_master_bus_config_t config = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = MARS_BOARD_PIN_I2C_SDA,
        .scl_io_num = MARS_BOARD_PIN_I2C_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    ESP_RETURN_ON_ERROR(i2c_new_master_bus(&config, &bus), TAG, "I2C init");
    codec_present = i2c_master_probe(bus, MARS_BOARD_CODEC_ADDRESS, 100) == ESP_OK;
    if (i2c_master_probe(bus, MARS_BOARD_BATTERY_ADDRESS, 100) == ESP_OK) {
        const i2c_device_config_t device = {
            .dev_addr_length = I2C_ADDR_BIT_LEN_7,
            .device_address = MARS_BOARD_BATTERY_ADDRESS,
            .scl_speed_hz = 100000,
        };
        ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(bus, &device, &gauge), TAG, "CW2017 attach");
    }
    return ESP_OK;
}

static esp_err_t init_keys(void)
{
    const adc_oneshot_unit_init_cfg_t unit = {.unit_id = ADC_UNIT_1};
    ESP_RETURN_ON_ERROR(adc_oneshot_new_unit(&unit, &adc), TAG, "ADC unit");
    const adc_oneshot_chan_cfg_t channel = {
        .atten = ADC_ATTEN_DB_12, .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    ESP_RETURN_ON_ERROR(adc_oneshot_config_channel(adc, ADC_CHANNEL_0, &channel), TAG, "ADC channel");
    const adc_cali_curve_fitting_config_t cal = {
        .unit_id = ADC_UNIT_1, .chan = ADC_CHANNEL_0, .atten = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    /* Thresholds are in mV, so never use uncalibrated raw counts as millivolts. */
    return adc_cali_create_scheme_curve_fitting(&cal, &calibration);
}

static esp_err_t init_display(void)
{
    const ledc_timer_config_t timer = {
        .speed_mode = LEDC_LOW_SPEED_MODE, .duty_resolution = LEDC_TIMER_10_BIT,
        .timer_num = LEDC_TIMER_0, .freq_hz = 5000, .clk_cfg = LEDC_AUTO_CLK,
    };
    ESP_RETURN_ON_ERROR(ledc_timer_config(&timer), TAG, "backlight timer");
    const ledc_channel_config_t backlight = {
        .gpio_num = MARS_BOARD_PIN_LCD_BACKLIGHT, .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = LEDC_CHANNEL_0, .timer_sel = LEDC_TIMER_0, .duty = 0,
    };
    ESP_RETURN_ON_ERROR(ledc_channel_config(&backlight), TAG, "backlight channel");
    const spi_bus_config_t spi = {
        .mosi_io_num = MARS_BOARD_PIN_LCD_MOSI, .miso_io_num = -1,
        .sclk_io_num = MARS_BOARD_PIN_LCD_SCLK, .quadwp_io_num = -1,
        .quadhd_io_num = -1, .max_transfer_sz = TRANSFER_PIXELS*sizeof(uint16_t),
    };
    ESP_RETURN_ON_ERROR(spi_bus_initialize(SPI2_HOST, &spi, SPI_DMA_CH_AUTO), TAG, "SPI init");
    color_done = xSemaphoreCreateBinary();
    ESP_RETURN_ON_FALSE(color_done, ESP_ERR_NO_MEM, TAG, "DMA semaphore");
    esp_lcd_panel_io_handle_t io = NULL;
    const esp_lcd_panel_io_spi_config_t io_config = {
        .cs_gpio_num = MARS_BOARD_PIN_LCD_CS, .dc_gpio_num = MARS_BOARD_PIN_LCD_DC,
        .pclk_hz = MARS_BOARD_DISPLAY_CLOCK_HZ, .spi_mode = 0,
        .trans_queue_depth = 1, .lcd_cmd_bits = 8, .lcd_param_bits = 8,
        .on_color_trans_done = transfer_done, .user_ctx = color_done,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_spi(SPI2_HOST, &io_config, &io), TAG, "LCD IO");
    const esp_lcd_panel_dev_config_t device = {
        .reset_gpio_num = -1, .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB, .bits_per_pixel = 16,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_st7789(io, &device, &panel), TAG, "ST7789 panel");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_reset(panel), TAG, "panel reset");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_init(panel), TAG, "panel init");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_disp_on_off(panel, false), TAG, "panel blank");
    /* Panel-specific porch/power/gamma values from the FoloToy reference. */
    static const struct { uint8_t cmd, length, data[14]; } commands[] = {
        {0xb2, 5, {5, 5, 0, 0x33, 0x33}}, {0xb7, 1, {0x35}}, {0xbb, 1, {0x21}},
        {0xc0, 1, {0x2c}}, {0xc2, 1, {1}}, {0xc3, 1, {0x0b}}, {0xc4, 1, {0x20}},
        {0xc6, 1, {0x0f}}, {0xd0, 2, {0xa7, 0xa1}}, {0xd0, 2, {0xa4, 0xa1}},
        {0xd6, 1, {0xa1}},
        {0xe0, 14, {0xd0, 4, 8, 0x0a, 9, 5, 0x2d, 0x43, 0x49, 9, 0x16, 0x15, 0x26, 0x2b}},
        {0xe1, 14, {0xd0, 3, 9, 0x0a, 0x0a, 6, 0x2e, 0x44, 0x40, 0x3a, 0x15, 0x15, 0x26, 0x2a}},
    };
    for (unsigned i = 0; i < sizeof(commands) / sizeof(commands[0]); ++i)
        ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, commands[i].cmd,
                            commands[i].data, commands[i].length), TAG, "panel command");
    vTaskDelay(pdMS_TO_TICKS(10));
    ESP_RETURN_ON_ERROR(esp_lcd_panel_invert_color(panel, true), TAG, "panel inversion");
    ESP_RETURN_ON_ERROR(mars_board_fill_rect(0, 0, MARS_BOARD_DISPLAY_WIDTH,
                        MARS_BOARD_DISPLAY_HEIGHT, 0), TAG, "startup clear");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_disp_on_off(panel, true), TAG, "display on");
    ESP_RETURN_ON_ERROR(ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, 512), TAG, "brightness");
    return ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
}

esp_err_t mars_board_init(void)
{
    if (initialized) return ESP_OK;
    if (init_attempted) return ESP_ERR_INVALID_STATE;
    init_attempted = true;
    ESP_RETURN_ON_ERROR(init_i2c(), TAG, "shared bus");
    ESP_RETURN_ON_ERROR(init_keys(), TAG, "keys");
    ESP_RETURN_ON_ERROR(init_display(), TAG, "display");
    initialized = true;
    return ESP_OK;
}

bool mars_board_touch_available(void) { return false; }
esp_err_t mars_board_i2c_bus_get(i2c_master_bus_handle_t *output)
{
    if (!output) return ESP_ERR_INVALID_ARG;
    if (!bus) return ESP_ERR_INVALID_STATE;
    *output = bus;
    return ESP_OK;
}
bool mars_board_codec_present(void) { return codec_present; }
bool mars_board_touch_read(uint16_t *x, uint16_t *y) { (void)x; (void)y; return false; }

int mars_board_key_read(void)
{
    int raw, mv;
    if (!adc || !calibration || adc_oneshot_read(adc, ADC_CHANNEL_0, &raw) != ESP_OK ||
        adc_cali_raw_to_voltage(calibration, raw, &mv) != ESP_OK) return 0;
    if (mv >= MARS_BOARD_KEY_1_MIN_MV && mv < MARS_BOARD_KEY_1_MAX_MV) return 1;
    if (mv >= MARS_BOARD_KEY_2_MIN_MV && mv < MARS_BOARD_KEY_2_MAX_MV) return 2;
    if (mv >= MARS_BOARD_KEY_3_MIN_MV && mv < MARS_BOARD_KEY_3_MAX_MV) return 3;
    return 0;
}

esp_err_t mars_board_battery_read(int *percent, int *millivolts)
{
    if (!percent || !millivolts) return ESP_ERR_INVALID_ARG;
    if (!gauge) return ESP_ERR_NOT_SUPPORTED;
    uint8_t reg = 2, voltage[2], soc[2];
    ESP_RETURN_ON_ERROR(i2c_master_transmit_receive(gauge, &reg, 1, voltage, 2, 100), TAG, "voltage");
    reg = 4;
    ESP_RETURN_ON_ERROR(i2c_master_transmit_receive(gauge, &reg, 1, soc, 2, 100), TAG, "SOC");
    /* Raw CW2017 telemetry only; cell profile and accuracy are not calibrated here. */
    *millivolts = ((((unsigned)voltage[0] << 8) | voltage[1]) & 0x3fff) * 5 / 16;
    *percent = soc[0] > 100 ? 100 : soc[0];
    return ESP_OK;
}

esp_err_t mars_board_blit(int x, int y, int w, int h, const uint16_t *pixels)
{
    if (!panel || !pixels || x < 0 || y < 0 || w <= 0 || h <= 0 ||
        w > MARS_BOARD_DISPLAY_WIDTH - x || h > MARS_BOARD_DISPLAY_HEIGHT - y ||
        w * h > TRANSFER_PIXELS) return ESP_ERR_INVALID_ARG;
    ESP_RETURN_ON_ERROR(esp_lcd_panel_draw_bitmap(panel, x, y, x + w, y + h, pixels), TAG, "draw");
    /* Timeout must be fatal: DMA may still own the caller's buffer. */
    ESP_ERROR_CHECK(xSemaphoreTake(color_done, pdMS_TO_TICKS(1000)) == pdTRUE ? ESP_OK : ESP_ERR_TIMEOUT);
    return ESP_OK;
}

esp_err_t mars_board_fill_rect(int x, int y, int w, int h, uint16_t color)
{
    if (x < 0 || y < 0 || w <= 0 || h <= 0 || w > MARS_BOARD_DISPLAY_WIDTH - x ||
        h > MARS_BOARD_DISPLAY_HEIGHT - y) return ESP_ERR_INVALID_ARG;
    int chunk=h<20?h:20;
    uint16_t *strip=heap_caps_malloc((size_t)w*chunk*sizeof(uint16_t),MALLOC_CAP_DMA|MALLOC_CAP_INTERNAL);
    if(!strip)return ESP_ERR_NO_MEM;
    esp_err_t result=ESP_OK;
    uint16_t wire = (uint16_t)((color << 8) | (color >> 8));
    for (int i = 0; i < w * chunk; ++i) strip[i] = wire;
    for (int row = 0; row < h; row += chunk) {
        int rows = h - row < chunk ? h - row : chunk;
        result=mars_board_blit(x,y+row,w,rows,strip);if(result!=ESP_OK)break;
    }
    free(strip);return result;
}

esp_err_t mars_board_brightness_set(unsigned percent)
{
 if(!initialized||percent>100)return ESP_ERR_INVALID_ARG;
 esp_err_t err=ledc_set_duty(LEDC_LOW_SPEED_MODE,LEDC_CHANNEL_0,percent*1023/100);
 return err==ESP_OK?ledc_update_duty(LEDC_LOW_SPEED_MODE,LEDC_CHANNEL_0):err;
}
