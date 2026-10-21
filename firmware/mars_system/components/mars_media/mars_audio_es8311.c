/* ES8311 backend extracted from the verified BOX implementation. */
#include "mars_audio_hw.h"
#include "mars_board.h"
#include "driver/i2s_std.h"
#include "esp_check.h"
#include "esp_codec_dev.h"
#include "esp_codec_dev_defaults.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#define I2S_BCLK_PIN MARS_BOARD_PIN_I2S_BCLK
#define I2S_WS_PIN MARS_BOARD_PIN_I2S_WS
#define I2S_DOUT_PIN MARS_BOARD_PIN_I2S_DOUT
#define I2S_DIN_PIN MARS_BOARD_PIN_I2S_DIN
#define ES8311_I2C_ADDR (MARS_BOARD_CODEC_ADDRESS << 1)
static const char *TAG="mars_audio_es8311";
static i2s_chan_handle_t s_tx_handle, s_rx_handle;
static const audio_codec_ctrl_if_t *s_ctrl_if;
static const audio_codec_data_if_t *s_data_if;
static const audio_codec_gpio_if_t *s_gpio_if;
static const audio_codec_if_t *s_codec_if;
static esp_codec_dev_handle_t s_dev;
static int s_input_rate, s_output_rate, s_volume;
static esp_err_t create_i2s_duplex(void)
{
    i2s_chan_config_t chan_cfg = {
        .id = I2S_NUM_0,
        .role = I2S_ROLE_MASTER,
        .dma_desc_num = 6,
        .dma_frame_num = 240,
        .auto_clear = true,
        .intr_priority = 0,
    };
    ESP_RETURN_ON_ERROR(i2s_new_channel(&chan_cfg, &s_tx_handle, &s_rx_handle), TAG,
                        "i2s_new_channel failed");
    i2s_std_config_t std_cfg = {
        .clk_cfg = {
            .sample_rate_hz = (uint32_t)s_output_rate,
            .clk_src = I2S_CLK_SRC_DEFAULT,
            .mclk_multiple = I2S_MCLK_MULTIPLE_256,
            .bclk_div = 8,
        },
        .slot_cfg = {
            .data_bit_width = I2S_DATA_BIT_WIDTH_16BIT,
            .slot_bit_width = I2S_SLOT_BIT_WIDTH_AUTO,
            .slot_mode = I2S_SLOT_MODE_STEREO,
            .slot_mask = I2S_STD_SLOT_BOTH,
            .ws_width = I2S_DATA_BIT_WIDTH_16BIT,
            .ws_pol = false,
            .bit_shift = true,
            .left_align = false,
            .big_endian = false,
            .bit_order_lsb = false,
        },
        .gpio_cfg = {
            .mclk = MARS_BOARD_PIN_I2S_MCLK,
            .bclk = I2S_BCLK_PIN,
            .ws = I2S_WS_PIN,
            .dout = I2S_DOUT_PIN,
            .din = I2S_DIN_PIN,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false,
            },
        },
    };
    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(s_tx_handle, &std_cfg), TAG,
                        "i2s tx init failed");
    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(s_rx_handle, &std_cfg), TAG,
                        "i2s rx init failed");
    /* 保持 READY：通道启停由 esp_codec_dev 管理，开机待机不发送 I2S 时钟。 */
    ESP_LOGI(TAG, "I2S duplex ready (bclk=%d ws=%d dout=%d din=%d)", I2S_BCLK_PIN,
             I2S_WS_PIN, I2S_DOUT_PIN, I2S_DIN_PIN);
    return ESP_OK;
}

static void reset_codec(void)
{
    /* ES8311 数字块复位（初始化指南推荐：拉复位寄存器保持数毫秒） */
    uint8_t reset_value = 0x1F;
    s_ctrl_if->write_reg(s_ctrl_if, 0x00, 1, &reset_value, 1);
    vTaskDelay(pdMS_TO_TICKS(5));
}

static esp_err_t init_codec_interfaces(void)
{
    /* I2S 数据通道 */
    audio_codec_i2s_cfg_t i2s_cfg = {
        .port = I2S_NUM_0,
        .rx_handle = s_rx_handle,
        .tx_handle = s_tx_handle,
    };
    s_data_if = audio_codec_new_i2s_data(&i2s_cfg);
    if (s_data_if == NULL) {
        return ESP_FAIL;
    }

    /* I2C 控制通道：复用 box3_board 的 I2C 总线（XL9555/触摸/ES8311 共享） */
    i2c_master_bus_handle_t i2c_bus = NULL;
    ESP_RETURN_ON_ERROR(mars_board_i2c_bus_get(&i2c_bus), TAG, "no shared I2C bus");
    audio_codec_i2c_cfg_t i2c_cfg = {
        .port = I2C_NUM_0,
        .addr = ES8311_I2C_ADDR,
        .bus_handle = i2c_bus,
        .clock_speed_hz = 100000,
    };
    s_ctrl_if = audio_codec_new_i2c_ctrl(&i2c_cfg);
    if (s_ctrl_if == NULL) {
        return ESP_FAIL;
    }
    reset_codec();

    /* 原板适配未配置独立 PA 控制脚；不要改动 LCD 的扩展 IO。 */
    s_gpio_if = audio_codec_new_gpio();
    if (s_gpio_if == NULL) {
        return ESP_FAIL;
    }

    es8311_codec_cfg_t es8311_cfg = {
        .ctrl_if = s_ctrl_if,
        .gpio_if = s_gpio_if,
        .codec_mode = ESP_CODEC_DEV_WORK_MODE_BOTH,
        .pa_pin = GPIO_NUM_NC,
        .pa_reverted = false,
        .master_mode = false,
        .use_mclk = false,
        .digital_mic = false,
        .invert_mclk = false,
        .invert_sclk = false,
        .hw_gain = {
            .pa_voltage = 5.0,
            .codec_dac_voltage = 3.3,
        },
        .no_dac_ref = false,
        .mclk_div = 0,
    };
    s_codec_if = es8311_codec_new(&es8311_cfg);
    if (s_codec_if == NULL) return ESP_FAIL;
    /* 构造器仅初始化寄存器，未承诺 DAC 静音。即使尚无 s_dev 也须静音。 */
    if (s_codec_if->mute(s_codec_if, true) != ESP_CODEC_DEV_OK) return ESP_FAIL;
    int mute_reg = 0;
    if (s_codec_if->get_reg(s_codec_if, 0x31, &mute_reg) != ESP_CODEC_DEV_OK ||
            (mute_reg & 0x60) != 0x60) return ESP_FAIL;
    ESP_LOGI(TAG, "idle DAC muted (reg31=0x%02x), I2S waits for audio owner", mute_reg);
    return ESP_OK;
}

esp_err_t mars_audio_hw_state(bool out_en, bool in_en, int volume)
{
    if (!s_codec_if) return ESP_ERR_INVALID_STATE;
    s_volume = volume;
    if ((out_en || in_en) && s_dev == NULL) {
        esp_codec_dev_cfg_t dev_cfg = {
            .dev_type = ESP_CODEC_DEV_TYPE_IN_OUT,
            .codec_if = s_codec_if,
            .data_if = s_data_if,
        };
        s_dev = esp_codec_dev_new(&dev_cfg);
        if (s_dev == NULL) {
            ESP_LOGE(TAG, "esp_codec_dev_new failed");
            return ESP_FAIL;
        }
        esp_codec_dev_sample_info_t fs = {
            .bits_per_sample = 16,
            .channel = 1,
            .channel_mask = 0,
            .sample_rate = (uint32_t)s_input_rate,
            .mclk_multiple = 0,
        };
        /* 先保存静音状态，open 的设置恢复步骤不会把纯录音输出留在非静音。 */
        esp_codec_dev_set_out_mute(s_dev, true);
        if (esp_codec_dev_open(s_dev, &fs) != ESP_OK) {
            ESP_LOGE(TAG, "esp_codec_dev_open failed");
            esp_codec_dev_delete(s_dev); s_dev = NULL; return ESP_FAIL;
        }
        esp_codec_dev_set_in_gain(s_dev, 30.0);
        esp_codec_dev_set_out_vol(s_dev, s_volume);
    } else if (!out_en && !in_en && s_dev != NULL) {
        esp_codec_dev_set_out_mute(s_dev, true);
        esp_codec_dev_close(s_dev);
        esp_codec_dev_delete(s_dev);
        s_dev = NULL;
    }
    /* IN_OUT 设备录音也会启用 DAC；仅输出持有者可取消静音。 */
    if (s_dev != NULL) {
        esp_codec_dev_set_out_mute(s_dev, !out_en);
    }
    int mute_reg = 0;
    if (s_codec_if->get_reg(s_codec_if, 0x31, &mute_reg) == ESP_CODEC_DEV_OK)
        ESP_LOGI(TAG, "audio state in=%d out=%d DAC reg31=0x%02x", in_en, out_en, mute_reg);
    return ESP_OK;
}


esp_err_t mars_audio_hw_init(int input_rate, int output_rate) {
    s_input_rate=input_rate; s_output_rate=output_rate;
    esp_err_t err=create_i2s_duplex();
    if (err==ESP_OK) err=init_codec_interfaces();
    if (err!=ESP_OK) mars_audio_hw_deinit();
    return err;
}
bool mars_audio_hw_active(void) { return s_dev != NULL; }
esp_err_t mars_audio_hw_volume(int volume) {
    s_volume=volume;
    return s_dev ? esp_codec_dev_set_out_vol(s_dev,volume) : ESP_OK;
}
esp_err_t mars_audio_hw_write(const int16_t *pcm,size_t samples,uint32_t timeout_ms) {
    (void)timeout_ms;
    return s_dev && esp_codec_dev_write(s_dev,(void *)pcm,samples*sizeof(int16_t))==ESP_CODEC_DEV_OK ? ESP_OK : ESP_FAIL;
}
esp_err_t mars_audio_hw_read(int16_t *pcm,size_t samples) {
    return s_dev && esp_codec_dev_read(s_dev,pcm,samples*sizeof(int16_t))==ESP_CODEC_DEV_OK ? ESP_OK : ESP_FAIL;
}
void mars_audio_hw_deinit(void) {
    if (s_dev) { esp_codec_dev_set_out_mute(s_dev,true); esp_codec_dev_close(s_dev); esp_codec_dev_delete(s_dev); s_dev=NULL; }
    if (s_codec_if) { audio_codec_delete_codec_if(s_codec_if); s_codec_if=NULL; }
    if (s_data_if) { audio_codec_delete_data_if(s_data_if); s_data_if=NULL; }
    if (s_ctrl_if) { audio_codec_delete_ctrl_if(s_ctrl_if); s_ctrl_if=NULL; }
    if (s_gpio_if) { audio_codec_delete_gpio_if(s_gpio_if); s_gpio_if=NULL; }
    if (s_tx_handle) { i2s_del_channel(s_tx_handle); s_tx_handle=NULL; }
    if (s_rx_handle) { i2s_del_channel(s_rx_handle); s_rx_handle=NULL; }
}
