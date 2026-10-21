/* Independent I2S clocks: PCM5101 DAC on I2S0, right-slot digital mic on I2S1. */
#include <stdatomic.h>
#include "mars_audio_hw.h"
#include "mars_board.h"
#include "driver/i2s_std.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "pcm5101_samples.h"
static const char *TAG="mars_audio_pcm5101";
static i2s_chan_handle_t tx,rx;
static bool tx_on,rx_on;
static atomic_int volume=60;

static esp_err_t create_channel(bool output,int rate,i2s_chan_handle_t *handle)
{
    i2s_chan_config_t chan=I2S_CHANNEL_DEFAULT_CONFIG(output ? I2S_NUM_0 : I2S_NUM_1,I2S_ROLE_MASTER);
    chan.dma_desc_num=6; chan.dma_frame_num=240; chan.auto_clear=true;
    ESP_RETURN_ON_ERROR(i2s_new_channel(&chan,output ? handle : NULL,output ? NULL : handle),TAG,"channel");
    i2s_std_config_t cfg={.clk_cfg=I2S_STD_CLK_DEFAULT_CONFIG(rate),
        .slot_cfg=I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT,I2S_SLOT_MODE_MONO),
        .gpio_cfg={.mclk=I2S_GPIO_UNUSED,
            .bclk=output ? MARS_BOARD_PIN_I2S_BCLK : MARS_BOARD_PIN_MIC_BCLK,
            .ws=output ? MARS_BOARD_PIN_I2S_WS : MARS_BOARD_PIN_MIC_WS,
            .dout=output ? MARS_BOARD_PIN_I2S_DOUT : I2S_GPIO_UNUSED,
            .din=output ? I2S_GPIO_UNUSED : MARS_BOARD_PIN_I2S_DIN}};
    cfg.slot_cfg.slot_mask=output ? I2S_STD_SLOT_LEFT : I2S_STD_SLOT_RIGHT;
    return i2s_channel_init_std_mode(*handle,&cfg);
}
esp_err_t mars_audio_hw_init(int input_rate,int output_rate)
{
    esp_err_t err=create_channel(true,output_rate,&tx);
    if (err==ESP_OK) err=create_channel(false,input_rate,&rx);
    if (err!=ESP_OK) { mars_audio_hw_deinit(); return err; }
    ESP_LOGI(TAG,"PCM5101 TX=%dHz, digital mic RX=%dHz; idle clocks stopped",output_rate,input_rate);
    return ESP_OK;
}
static esp_err_t enable(i2s_chan_handle_t channel,bool want,bool *on)
{
    if (want==*on) return ESP_OK;
    esp_err_t err=want ? i2s_channel_enable(channel) : i2s_channel_disable(channel);
    if (err==ESP_OK) *on=want;
    return err;
}
esp_err_t mars_audio_hw_state(bool output,bool input,int percent)
{
    if (!tx || !rx) return ESP_ERR_INVALID_STATE;
    atomic_store(&volume,percent);
    ESP_RETURN_ON_ERROR(enable(tx,output,&tx_on),TAG,"TX state");
    return enable(rx,input,&rx_on);
}
bool mars_audio_hw_active(void) { return tx && rx; }
esp_err_t mars_audio_hw_volume(int percent) { atomic_store(&volume,percent); return ESP_OK; }
esp_err_t mars_audio_hw_write(const int16_t *pcm,size_t samples,uint32_t timeout_ms)
{
    if (!tx_on || !pcm) return ESP_ERR_INVALID_STATE;
    int32_t buffer[256];
    /* Timeout is a total deadline; a zero API timeout means the normal blocking mode. */
    int64_t deadline=esp_timer_get_time()+(int64_t)timeout_ms*1000;
    for (size_t offset=0;offset<samples;) {
        size_t count=samples-offset; if (count>256) count=256;
        int gain=atomic_load(&volume);
        for (size_t i=0;i<count;++i) buffer[i]=pcm5101_output_sample(pcm[offset+i],gain);
        size_t written=0;
        int64_t left=timeout_ms ? deadline-esp_timer_get_time() : 2000000;
        if (left<=0) return ESP_ERR_TIMEOUT;
        esp_err_t err=i2s_channel_write(tx,buffer,count*sizeof(int32_t),&written,(left+999)/1000);
        if (err!=ESP_OK) return err;
        if (written!=count*sizeof(int32_t)) return ESP_FAIL;
        offset+=count;
    }
    return ESP_OK;
}
esp_err_t mars_audio_hw_read(int16_t *pcm,size_t samples)
{
    if (!rx_on || !pcm) return ESP_ERR_INVALID_STATE;
    int32_t buffer[256];
    for (size_t offset=0;offset<samples;) {
        size_t count=samples-offset; if (count>256) count=256;
        size_t received=0;
        esp_err_t err=i2s_channel_read(rx,buffer,count*sizeof(int32_t),&received,200);
        if (err!=ESP_OK) return err;
        if (received!=count*sizeof(int32_t)) return ESP_FAIL;
        for (size_t i=0;i<count;++i) pcm[offset+i]=pcm5101_input_sample(buffer[i]);
        offset+=count;
    }
    return ESP_OK;
}
void mars_audio_hw_deinit(void)
{
    if (tx) { if (tx_on) i2s_channel_disable(tx); i2s_del_channel(tx); tx=NULL; }
    if (rx) { if (rx_on) i2s_channel_disable(rx); i2s_del_channel(rx); rx=NULL; }
    tx_on=rx_on=false;
}
