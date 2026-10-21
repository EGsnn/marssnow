#include "box3_board.h"

#include <stdio.h>

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_attr.h"
#include "esp_check.h"
#include "esp_lcd_io_i80.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#define BOX_XL9555_ADDR    0x20
#define BOX_CHSC5432_ADDR  0x2e
#define CHSC5432_PID_REG   0x20000080U
#define CHSC5432_TOUCH_REG 0x2000002cU

#define AW_OUTPUT0         0x02
static const char *TAG = "box_board";
static i2c_master_bus_handle_t s_i2c_bus;
static i2c_master_dev_handle_t s_expander;
static i2c_master_dev_handle_t s_touch;
static esp_lcd_panel_handle_t s_panel;
static SemaphoreHandle_t s_color_done;
static uint16_t s_expander_output;
DMA_ATTR static uint16_t s_line_buffer[BOX3_LCD_WIDTH * 20];

/* 供 mars_media 等共享 I2C 总线（不重复创建 i2c_master_bus） */
esp_err_t box3_i2c_bus_get(i2c_master_bus_handle_t *bus)
{
    if (bus == NULL || s_i2c_bus == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    *bus = s_i2c_bus;
    return ESP_OK;
}

static bool color_trans_done(esp_lcd_panel_io_handle_t io,
                             esp_lcd_panel_io_event_data_t *event, void *ctx)
{
    BaseType_t wake = pdFALSE;
    xSemaphoreGiveFromISR((SemaphoreHandle_t)ctx, &wake);
    return wake == pdTRUE;
}

static esp_err_t expander_write(uint8_t reg, const uint8_t *data, size_t len)
{
    uint8_t payload[3];
    if (len > 2) return ESP_ERR_INVALID_SIZE;
    payload[0] = reg;
    for (size_t i = 0; i < len; ++i) payload[i + 1] = data[i];
    return i2c_master_transmit(s_expander, payload, len + 1, 100);
}

static esp_err_t expander_read(uint8_t reg, uint8_t *data, size_t len)
{
    return i2c_master_transmit_receive(s_expander, &reg, 1, data, len, 100);
}

static esp_err_t expander_set_output(uint16_t mask, bool high)
{
    if (high) s_expander_output |= mask;
    else s_expander_output &= (uint16_t)~mask;
    uint8_t data[2] = {(uint8_t)s_expander_output, (uint8_t)(s_expander_output >> 8)};
    return expander_write(AW_OUTPUT0, data, sizeof(data));
}

static esp_err_t new_i2c_bus(int sda, int scl)
{
    const i2c_master_bus_config_t config = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = sda,
        .scl_io_num = scl,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    return i2c_new_master_bus(&config, &s_i2c_bus);
}

static esp_err_t attach_i2c_device(uint8_t address, i2c_master_dev_handle_t *device)
{
    const i2c_device_config_t config = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = address,
        .scl_speed_hz = 400000,
    };
    return i2c_master_bus_add_device(s_i2c_bus, &config, device);
}

static esp_err_t touch_read_reg(uint32_t reg, uint8_t *data, size_t len)
{
    const uint8_t address[4] = {
        (uint8_t)(reg >> 24), (uint8_t)(reg >> 16), (uint8_t)(reg >> 8), (uint8_t)reg,
    };
    return i2c_master_transmit_receive(s_touch, address, sizeof(address), data, len, 100);
}

static void init_box_touch(void)
{
    /* The touch panel shares the board I2C bus.  Do not make it fatal: some
     * ATK BOX bundles were sold with the display-only module. */
    if (i2c_master_probe(s_i2c_bus, BOX_CHSC5432_ADDR, 100) != ESP_OK) {
        ESP_LOGW(TAG, "CHSC5432 touch not detected at I2C 0x%02x", BOX_CHSC5432_ADDR);
        return;
    }
    if (attach_i2c_device(BOX_CHSC5432_ADDR, &s_touch) != ESP_OK) {
        s_touch = NULL;
        ESP_LOGW(TAG, "CHSC5432 attach failed");
        return;
    }
    uint8_t id[4] = {0};
    if (touch_read_reg(CHSC5432_PID_REG, id, sizeof(id)) == ESP_OK) {
        ESP_LOGI(TAG, "CHSC5432 touch ready: id=%02x%02x%02x%02x", id[0], id[1], id[2], id[3]);
    } else {
        ESP_LOGI(TAG, "CHSC5432 touch ready at I2C 0x%02x", BOX_CHSC5432_ADDR);
    }
}

static esp_err_t init_box_control(void)
{
    ESP_RETURN_ON_ERROR(new_i2c_bus(MARS_BOARD_PIN_I2C_SDA, MARS_BOARD_PIN_I2C_SCL), TAG, "BOX I2C init failed");
    ESP_RETURN_ON_ERROR(i2c_master_probe(s_i2c_bus, BOX_XL9555_ADDR, 100), TAG, "XL9555 not found");
    ESP_RETURN_ON_ERROR(attach_i2c_device(BOX_XL9555_ADDR, &s_expander), TAG, "XL9555 attach failed");
    const uint8_t directions[2] = {0x1b, 0xfe};
    ESP_RETURN_ON_ERROR(expander_write(0x06, directions, 2), TAG, "XL9555 direction failed");
    uint8_t outputs[2] = {0};
    ESP_RETURN_ON_ERROR(expander_read(0x02, outputs, 2), TAG, "XL9555 output read failed");
    s_expander_output = (uint16_t)outputs[0] | ((uint16_t)outputs[1] << 8);
    ESP_LOGI(TAG, "XL9555 before display: output=%02x%02x", outputs[1], outputs[0]);
    return ESP_OK;
}

static esp_err_t enable_box_display_power(void)
{
    ESP_RETURN_ON_ERROR(expander_set_output(1u << 5, true), TAG, "XL9555 output 5 enable failed");
    ESP_RETURN_ON_ERROR(expander_set_output(1u << 7, true), TAG, "XL9555 output 7 enable failed");
    uint8_t outputs[2] = {0};
    uint8_t inputs[2] = {0};
    uint8_t directions[2] = {0};
    ESP_RETURN_ON_ERROR(expander_read(0x02, outputs, 2), TAG, "XL9555 output verify failed");
    ESP_RETURN_ON_ERROR(expander_read(0x00, inputs, 2), TAG, "XL9555 input verify failed");
    ESP_RETURN_ON_ERROR(expander_read(0x06, directions, 2), TAG, "XL9555 config verify failed");
    ESP_LOGI(TAG, "XL9555 display enabled: output=%02x%02x input=%02x%02x config=%02x%02x",
             outputs[1], outputs[0], inputs[1], inputs[0], directions[1], directions[0]);
    return ESP_OK;
}

static esp_err_t finish_panel_init(esp_lcd_panel_io_handle_t io)
{
    esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = -1,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_st7789(io, &panel_config, &s_panel), TAG, "ST7789 init failed");
    /* ST7789 GRAM survives an ESP32 soft reset. Hide it before reinitializing
     * the controller so the previous App is not exposed during startup. */
    ESP_RETURN_ON_ERROR(esp_lcd_panel_disp_on_off(s_panel, false), TAG, "LCD blank failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_reset(s_panel), TAG, "LCD reset failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_init(s_panel), TAG, "LCD panel init failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_disp_on_off(s_panel, false), TAG, "LCD reblank failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_invert_color(s_panel, true), TAG, "LCD invert failed");
    {
        const uint8_t madctl = 0x00;
        const uint8_t pixel_format = 0x65;
        ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, 0x36, &madctl, 1), TAG, "LCD MADCTL failed");
        ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, 0x3a, &pixel_format, 1), TAG,
                            "LCD pixel format failed");
    }
    ESP_RETURN_ON_ERROR(esp_lcd_panel_swap_xy(s_panel, true), TAG, "LCD rotation failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_mirror(s_panel, true, false), TAG, "LCD mirror failed");
    ESP_RETURN_ON_ERROR(box3_display_fill_rect(0, 0, BOX3_LCD_WIDTH, BOX3_LCD_HEIGHT, 0x0000),
                        TAG, "LCD startup clear failed");
    return esp_lcd_panel_disp_on_off(s_panel, true);
}

static esp_err_t init_box_display(void)
{
    /* GPIO2 is LCD D/C on this board. Never probe BOX3 I2C on this pin.
     * Set push-pull explicitly before routing the LCD peripheral signals. */
    const gpio_config_t dc_config = {
        .pin_bit_mask = 1ULL << MARS_BOARD_PIN_LCD_DC,
        .mode = GPIO_MODE_OUTPUT,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&dc_config), TAG, "LCD DC push-pull init failed");
    const gpio_config_t rd_config = {
        .pin_bit_mask = 1ULL << MARS_BOARD_PIN_LCD_RD,
        .mode = GPIO_MODE_INPUT_OUTPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&rd_config), TAG, "LCD RD init failed");
    gpio_set_level(MARS_BOARD_PIN_LCD_RD, 1);
    esp_lcd_i80_bus_handle_t bus = NULL;
    const esp_lcd_i80_bus_config_t bus_config = {
        .dc_gpio_num = MARS_BOARD_PIN_LCD_DC,
        .wr_gpio_num = MARS_BOARD_PIN_LCD_WR,
        .clk_src = LCD_CLK_SRC_DEFAULT,
        .data_gpio_nums = {MARS_BOARD_PIN_LCD_D0, MARS_BOARD_PIN_LCD_D1, MARS_BOARD_PIN_LCD_D2,
                          MARS_BOARD_PIN_LCD_D3, MARS_BOARD_PIN_LCD_D4, MARS_BOARD_PIN_LCD_D5,
                          MARS_BOARD_PIN_LCD_D6, MARS_BOARD_PIN_LCD_D7},
        .bus_width = 8,
        .max_transfer_bytes = BOX3_LCD_WIDTH * BOX3_LCD_HEIGHT * sizeof(uint16_t),
        .dma_burst_size = 64,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_i80_bus(&bus_config, &bus), TAG, "BOX LCD bus init failed");
    s_color_done = xSemaphoreCreateBinary();
    ESP_RETURN_ON_FALSE(s_color_done, ESP_ERR_NO_MEM, TAG, "DMA completion semaphore failed");
    esp_lcd_panel_io_handle_t io = NULL;
    const esp_lcd_panel_io_i80_config_t io_config = {
        .cs_gpio_num = MARS_BOARD_PIN_LCD_CS,
        .pclk_hz = MARS_BOARD_DISPLAY_CLOCK_HZ,
        .trans_queue_depth = 10,
        .on_color_trans_done = color_trans_done,
        .user_ctx = s_color_done,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
        .dc_levels = {.dc_idle_level = 0, .dc_cmd_level = 0, .dc_dummy_level = 0, .dc_data_level = 1},
        .flags.swap_color_bytes = 0,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_i80(bus, &io_config, &io), TAG, "BOX LCD IO init failed");
    ESP_RETURN_ON_ERROR(gpio_dump_io_configuration(stdout, 1ULL << MARS_BOARD_PIN_LCD_DC), TAG, "DC GPIO diagnostic failed");
    return finish_panel_init(io);
}

esp_err_t box3_board_init(void)
{
    ESP_LOGI(TAG, "Fixed board: ATK-DNESP32S3-BOX, I80, no BOX3 probe");
    ESP_RETURN_ON_ERROR(init_box_control(), TAG, "ATK BOX controller not found");
    const gpio_config_t button_config = {
        .pin_bit_mask = 1ULL << MARS_BOARD_PIN_KEY,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&button_config), TAG, "button init failed");
    ESP_RETURN_ON_ERROR(init_box_display(), TAG, "display init failed");
    ESP_RETURN_ON_ERROR(enable_box_display_power(), TAG, "display power enable failed");
    vTaskDelay(pdMS_TO_TICKS(100));
    init_box_touch();
    ESP_LOGI(TAG, "ATK BOX initialized; pixel transfers wait for DMA completion");
    return ESP_OK;
}

esp_err_t box3_display_blit(int x, int y, int width, int height, const uint16_t *pixels)
{
    if (!s_panel || !pixels || x < 0 || y < 0 || width <= 0 || height <= 0 ||
        x + width > BOX3_LCD_WIDTH || y + height > BOX3_LCD_HEIGHT) return ESP_ERR_INVALID_ARG;
    esp_err_t result = esp_lcd_panel_draw_bitmap(s_panel, x, y, x + width, y + height, pixels);
    if (result != ESP_OK) {
        ESP_LOGE(TAG, "LCD transfer rejected: %s", esp_err_to_name(result));
        return result;
    }
    /* Single drawing task: do not let it reuse pixels until DMA signals done.
     * A timeout is fatal because the buffer may still belong to the hardware. */
    if (xSemaphoreTake(s_color_done, pdMS_TO_TICKS(1000)) != pdTRUE) {
        ESP_LOGE(TAG, "DMA timeout: x=%d y=%d w=%d h=%d", x, y, width, height);
        ESP_ERROR_CHECK(ESP_ERR_TIMEOUT);
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}

esp_err_t box3_display_fill_rect(int x, int y, int width, int height, uint16_t color)
{
    if (x < 0 || y < 0 || width <= 0 || height <= 0 || x + width > BOX3_LCD_WIDTH ||
        y + height > BOX3_LCD_HEIGHT || width > BOX3_LCD_WIDTH) return ESP_ERR_INVALID_ARG;
    const int rows_per_chunk = (int)(sizeof(s_line_buffer) / sizeof(s_line_buffer[0])) / width;
    const uint16_t wire_color = (uint16_t)((color << 8) | (color >> 8));
    for (int i = 0; i < width * rows_per_chunk; ++i) s_line_buffer[i] = wire_color;
    for (int row = 0; row < height; row += rows_per_chunk) {
        const int rows = (height - row < rows_per_chunk) ? height - row : rows_per_chunk;
        ESP_RETURN_ON_ERROR(box3_display_blit(x, y + row, width, rows, s_line_buffer), TAG,
                            "LCD fill failed");
    }
    return ESP_OK;
}

bool box3_touch_available(void) { return s_touch != NULL; }

bool box3_touch_read(uint16_t *x, uint16_t *y)
{
    if (!s_touch || !x || !y) return false;

    uint8_t data[28] = {0};
    if (touch_read_reg(CHSC5432_TOUCH_REG, data, sizeof(data)) != ESP_OK) return false;
    const uint8_t points = data[1] & 0x0f;
    if (points == 0 || points > 5) return false;

    /* The LCD is mounted in landscape mode.  This is the coordinate transform
     * used by Alientek's CHSC5xxx example for the same 320x240 panel. */
    const uint16_t raw_x = ((uint16_t)(data[5] >> 4) << 8) | data[3];
    const uint16_t raw_y = ((uint16_t)(data[5] & 0x0f) << 8) | data[2];
    if (raw_x >= BOX3_LCD_WIDTH || raw_y > BOX3_LCD_HEIGHT) return false;
    *x = raw_x;
    *y = raw_y == 0 ? 0 : BOX3_LCD_HEIGHT - raw_y;
    return *y < BOX3_LCD_HEIGHT;
}

int box3_key_read(void)
{
    if (gpio_get_level(MARS_BOARD_PIN_KEY) == 0) return 3;
    uint8_t inputs = 0xff;
    if (s_expander && expander_read(0x00, &inputs, 1) == ESP_OK) {
        if ((inputs & 0x01) == 0) return 1;
        if ((inputs & 0x02) == 0) return 2;
    }
    return 0;
}
