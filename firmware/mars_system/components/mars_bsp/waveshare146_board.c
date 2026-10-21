/* Waveshare ESP32-S3-Touch-LCD-1.46. Official reference pins, independent BSP. */
#include <string.h>
#include "mars_board.h"
#include "mars_slide.h"
#include "esp_timer.h"
#include "box3_board.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_io_interface.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_spd2010.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#define W MARS_BOARD_DISPLAY_WIDTH
#define H MARS_BOARD_DISPLAY_HEIGHT
#define STRIP_ROWS 16
#define RESET_MASK ((1U<<MARS_BOARD_TOUCH_RESET_BIT)|(1U<<MARS_BOARD_DISPLAY_RESET_BIT))
static const char *TAG = "waveshare146";
static i2c_master_bus_handle_t bus;
static i2c_master_dev_handle_t expander;
static esp_lcd_panel_handle_t panel;
static esp_lcd_panel_io_handle_t panel_io;
static spi_device_handle_t stream_device;
static SemaphoreHandle_t done;
static uint16_t *shadow, *strip;
static bool initialized, display_failed;
esp_err_t waveshare146_touch_init(i2c_master_bus_handle_t bus);
bool waveshare146_touch_read(uint16_t *x, uint16_t *y);
mars_touch_sample_t waveshare146_touch_sample(void);
bool waveshare146_touch_available(void);

/* Public panel-IO extension, sharing our one SPI device with the compositor.
 * No access to ESP-IDF private layouts; one hardware CS route for both paths. */
static esp_err_t io_param(esp_lcd_panel_io_t *io,int cmd,const void *data,size_t size)
{
    (void)io;if(cmd==-1&&!size)return ESP_OK;
    spi_transaction_ext_t t={0};t.base.flags=SPI_TRANS_VARIABLE_ADDR;
    t.address_bits=cmd==-1?0:32;t.base.addr=(uint32_t)cmd;
    t.base.length=size*8;t.base.tx_buffer=size?data:NULL;
    return spi_device_polling_transmit(stream_device,&t.base);
}
static esp_err_t io_color(esp_lcd_panel_io_t *io,int cmd,const void *data,size_t size)
{
    (void)io;
    esp_err_t err=spi_device_acquire_bus(stream_device,portMAX_DELAY);
    if(err!=ESP_OK)return err;
    const uint8_t *pixels=data;bool first=true;
    while(size&&err==ESP_OK){
        size_t chunk=size;if(chunk>W*8*2)chunk=W*8*2;
        spi_transaction_ext_t t={0};
        t.base.flags=SPI_TRANS_MODE_QIO|SPI_TRANS_VARIABLE_ADDR;
        if(chunk<size)t.base.flags|=SPI_TRANS_CS_KEEP_ACTIVE;
        t.address_bits=first&&cmd!=-1?32:0;t.base.addr=(uint32_t)cmd;
        t.base.length=chunk*8;t.base.tx_buffer=pixels;
        err=spi_device_polling_transmit(stream_device,&t.base);
        pixels+=chunk;size-=chunk;first=false;
    }
    spi_device_release_bus(stream_device);
    if(err==ESP_OK)xSemaphoreGive(done); /* Synchronous transport, task context. */
    return err;
}
static esp_err_t io_read(esp_lcd_panel_io_t *io,int cmd,void *data,size_t size)
{(void)io;(void)cmd;(void)data;(void)size;return ESP_ERR_NOT_SUPPORTED;}
static esp_err_t io_delete(esp_lcd_panel_io_t *io)
{(void)io;return spi_bus_remove_device(stream_device);}
static esp_err_t io_callbacks(esp_lcd_panel_io_t *io,const esp_lcd_panel_io_callbacks_t *cb,void *ctx)
{(void)io;(void)cb;(void)ctx;return ESP_ERR_NOT_SUPPORTED;}
static esp_lcd_panel_io_t owned_io={.tx_param=io_param,.tx_color=io_color,
    .rx_param=io_read,.del=io_delete,.register_event_callbacks=io_callbacks};

static esp_err_t expander_update(uint8_t reg, uint8_t mask, uint8_t value)
{
    esp_err_t err=ESP_FAIL;
    for(unsigned attempt=0;attempt<4;++attempt){
        uint8_t old;
        err=i2c_master_transmit_receive(expander,&reg,1,&old,1,100);
        if(err==ESP_OK){
            uint8_t command[]={reg,(old&~mask)|(value&mask)};
            err=i2c_master_transmit(expander,command,sizeof(command),100);
            if(err==ESP_OK)return ESP_OK;
        }
        /* A failed transfer never supplies a register value. Read afresh on
         * each retry and preserve every output outside the reset-pin mask. */
        if(attempt<3)vTaskDelay(pdMS_TO_TICKS(10*(attempt+1)));
    }
    return err;
}

static bool bounds(int x, int y, int w, int h)
{ return x >= 0 && y >= 0 && w > 0 && h > 0 && w <= W && h <= H && x <= W-w && y <= H-h; }

/* SPD2010 requires x start/end on 4-pixel boundaries. Preserve neighbors in
 * a big-endian PSRAM shadow; never enlarge a caller's source buffer read. */
/* esp_lcd queues bounded internal DMA bounce chunks from PSRAM. Drain the
 * queue after the final ISR so its temporary SRAM is returned before app load. */
static esp_err_t transfer_done(void)
{
    if (xSemaphoreTake(done,pdMS_TO_TICKS(2000)) != pdTRUE) {
        display_failed=true;return ESP_ERR_TIMEOUT;
    }
    esp_err_t err=esp_lcd_panel_io_tx_param(panel_io,-1,NULL,0);
    if(err!=ESP_OK)display_failed=true;
    return err;
}
static esp_err_t flush(int x, int y, int w, int h)
{
    if (!initialized || display_failed) return ESP_ERR_INVALID_STATE;
    int left = x & ~3, right = (x+w+3) & ~3;
    int width = right-left;
    for (int row = y; row < y+h; row += STRIP_ROWS) {
        int rows = y+h-row;
        if (rows > STRIP_ROWS) rows = STRIP_ROWS;
        for (int i=0; i<rows; ++i)
            memcpy(strip+i*width, shadow+(row+i)*W+left, width*sizeof(uint16_t));
        esp_err_t err = esp_lcd_panel_draw_bitmap(panel, left, row, right, row+rows, strip);
        if (err != ESP_OK) { display_failed = true; return err; }
        err=transfer_done();
        if(err!=ESP_OK)return err;
    }
    return ESP_OK;
}

esp_err_t mars_board_fill_rect(int x, int y, int w, int h, uint16_t color)
{
    if (!bounds(x,y,w,h)) return ESP_ERR_INVALID_ARG;
    if (!initialized) return ESP_ERR_INVALID_STATE;
    uint16_t wire = __builtin_bswap16(color);
    for (int row=y; row<y+h; ++row)
        for (int col=x; col<x+w; ++col) shadow[row*W+col] = wire;
    return flush(x,y,w,h);
}
esp_err_t mars_board_blit(int x, int y, int w, int h, const uint16_t *pixels)
{
    if (!pixels || !bounds(x,y,w,h)) return ESP_ERR_INVALID_ARG;
    if (!initialized) return ESP_ERR_INVALID_STATE;
    for (int row=0; row<h; ++row)
        memcpy(shadow+(y+row)*W+x, pixels+row*w, w*sizeof(uint16_t));
    return flush(x,y,w,h);
}

/* Native animation keeps one RAMWR window open while two 8-row internal
 * buffers alternate. Compose buffer N+1 while DMA sends N. PSRAM is only read;
 * no full intermediate frame is written on every animation step. The legacy
 * panel IO and compositor share one SPI device, CS and the single UI owner. */
static esp_err_t stream_slide(const uint16_t *next,const uint16_t *old,int offset,bool swap)
{
    uint16_t *buffers[2]={
        heap_caps_malloc(W*8*2,MALLOC_CAP_DMA|MALLOC_CAP_INTERNAL),
        heap_caps_malloc(W*8*2,MALLOC_CAP_DMA|MALLOC_CAP_INTERNAL)};
    if(!buffers[0]||!buffers[1]){free(buffers[0]);free(buffers[1]);return ESP_ERR_NO_MEM;}
    uint8_t area[4]={0,0,(W-1)>>8,(W-1)&255};
    esp_err_t err=esp_lcd_panel_io_tx_param(panel_io,0x02002a00,area,4);
    if(err==ESP_OK)err=esp_lcd_panel_io_tx_param(panel_io,0x02002b00,area,4);
    if(err!=ESP_OK){free(buffers[0]);free(buffers[1]);return err;}
    err=spi_device_acquire_bus(stream_device,portMAX_DELAY);
    if(err!=ESP_OK){free(buffers[0]);free(buffers[1]);return err;}
    spi_transaction_ext_t transactions[2]={0};
    unsigned queued=0,pending=0;
    for(int y=0;y<H&&err==ESP_OK;y+=8){
        if(pending==2){
            spi_transaction_t *completed;
            err=spi_device_get_trans_result(stream_device,&completed,portMAX_DELAY);
            --pending;if(err!=ESP_OK)break;
        }
        unsigned slot=queued%2;int rows=H-y;if(rows>8)rows=8;
        for(int row=0;row<rows;++row){
            if(swap){const uint16_t *src=next+(y+row)*W;uint16_t *dst=buffers[slot]+row*W;
                for(int x=0;x<W;++x)dst[x]=__builtin_bswap16(src[x]);
            }else mars_slide_row(buffers[slot]+row*W,next+(y+row)*W,old+(y+row)*W,W,offset);
        }
        spi_transaction_ext_t *t=transactions+slot;
        memset(t,0,sizeof(*t));
        /* 32 single-lane address bits encode the same 0x32/0x002c00
         * command as SPD2010's panel IO. Following chunks have no command;
         * CS stays asserted until the final chunk, data is always quad. */
        t->base.flags=SPI_TRANS_MODE_QIO|SPI_TRANS_VARIABLE_ADDR;
        if(y+rows<H)t->base.flags|=SPI_TRANS_CS_KEEP_ACTIVE;
        t->address_bits=y?0:32;t->base.addr=0x32002c00;
        t->base.length=rows*W*16;t->base.tx_buffer=buffers[slot];
        err=spi_device_queue_trans(stream_device,&t->base,portMAX_DELAY);
        if(err==ESP_OK){++queued;++pending;}
    }
    while(pending){
        spi_transaction_t *completed;
        esp_err_t result=spi_device_get_trans_result(stream_device,&completed,portMAX_DELAY);
        --pending;if(err==ESP_OK)err=result;
    }
    spi_device_release_bus(stream_device);
    free(buffers[0]);free(buffers[1]);
    return err;
}
esp_err_t mars_board_present_slide(const uint16_t *next,const uint16_t *old,int offset)
{
    if (!next || !old || offset < -W || offset > W) return ESP_ERR_INVALID_ARG;
    if (!initialized || display_failed) return ESP_ERR_INVALID_STATE;
    esp_err_t err=stream_device?stream_slide(next,old,offset,false):ESP_ERR_NO_MEM;
    if(err==ESP_OK){
        /* No other drawing may interleave with a transition. Sync alignment
         * neighbors at its completed endpoint before legacy partial updates. */
        if(!offset)memcpy(shadow,next,W*H*2);
        return ESP_OK;
    }
    if(err!=ESP_ERR_NO_MEM){display_failed=true;return err;}
    /* Allocation/device fallback keeps the already verified bulk path. */
    for(int y=0;y<H;++y)mars_slide_row(shadow+y*W,next+y*W,old+y*W,W,offset);
    err=esp_lcd_panel_draw_bitmap(panel,0,0,W,H,shadow);
    if(err!=ESP_OK){display_failed=true;return err;}
    return transfer_done();
}

esp_err_t mars_board_present_cpu_frame(const uint16_t *pixels)
{
    if(!pixels)return ESP_ERR_INVALID_ARG;
    if(!initialized||display_failed)return ESP_ERR_INVALID_STATE;
    esp_err_t err=stream_slide(pixels,pixels,0,true);
    if(err==ESP_ERR_NO_MEM){
        for(int i=0;i<W*H;++i)shadow[i]=__builtin_bswap16(pixels[i]);
        err=esp_lcd_panel_draw_bitmap(panel,0,0,W,H,shadow);
        if(err==ESP_OK)err=transfer_done();
    }
    if(err!=ESP_OK)display_failed=true;
    return err;
}

esp_err_t mars_board_init(void)
{
    if (initialized) return ESP_OK;
    gpio_config_t power = {.pin_bit_mask = 1ULL<<MARS_BOARD_PIN_POWER_ENABLE, .mode=GPIO_MODE_OUTPUT};
    ESP_RETURN_ON_ERROR(gpio_config(&power), TAG, "power pin");
    ESP_RETURN_ON_ERROR(gpio_set_level(MARS_BOARD_PIN_POWER_ENABLE, 1), TAG, "power latch");
    gpio_config_t light = {.pin_bit_mask = 1ULL<<MARS_BOARD_PIN_LCD_BACKLIGHT, .mode=GPIO_MODE_OUTPUT};
    ESP_RETURN_ON_ERROR(gpio_config(&light), TAG, "backlight");
    gpio_set_level(MARS_BOARD_PIN_LCD_BACKLIGHT, 0);
    gpio_config_t buttons = {.pin_bit_mask=(1ULL<<MARS_BOARD_PIN_KEY)|(1ULL<<MARS_BOARD_PIN_POWER_KEY),
        .mode=GPIO_MODE_INPUT, .pull_up_en=GPIO_PULLUP_ENABLE};
    ESP_RETURN_ON_ERROR(gpio_config(&buttons), TAG, "buttons");
    i2c_master_bus_config_t cfg = {.i2c_port=I2C_NUM_0, .sda_io_num=MARS_BOARD_PIN_I2C_SDA,
        .scl_io_num=MARS_BOARD_PIN_I2C_SCL, .clk_source=I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt=7, .flags.enable_internal_pullup=true};
    ESP_RETURN_ON_ERROR(i2c_new_master_bus(&cfg, &bus), TAG, "I2C");
    i2c_device_config_t expcfg = {.dev_addr_length=I2C_ADDR_BIT_LEN_7, .device_address=MARS_BOARD_EXPANDER_ADDRESS, .scl_speed_hz=100000};
    ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(bus,&expcfg,&expander), TAG, "TCA9554");
    /* Vendor EXIO1/EXIO2 are one-based names: touch=bit0, LCD=bit1.
     * Change only those two pins; retain all other expander outputs/directions. */
    ESP_RETURN_ON_ERROR(expander_update(1,RESET_MASK,RESET_MASK), TAG, "reset idle");
    ESP_RETURN_ON_ERROR(expander_update(3,RESET_MASK,0), TAG, "reset outputs");
    ESP_RETURN_ON_ERROR(expander_update(1,RESET_MASK,0), TAG, "reset low");
    vTaskDelay(pdMS_TO_TICKS(100));
    ESP_RETURN_ON_ERROR(expander_update(1,RESET_MASK,RESET_MASK), TAG, "reset high");
    vTaskDelay(pdMS_TO_TICKS(100));
    shadow=heap_caps_calloc(W*H,sizeof(uint16_t),MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
    strip=heap_caps_malloc(W*STRIP_ROWS*sizeof(uint16_t),MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
    done=xSemaphoreCreateBinary();
    if (!shadow || !strip || !done) return ESP_ERR_NO_MEM;
    spi_bus_config_t spi = {.sclk_io_num=MARS_BOARD_PIN_LCD_SCLK,
        .data0_io_num=MARS_BOARD_PIN_LCD_D0, .data1_io_num=MARS_BOARD_PIN_LCD_D1,
        .data2_io_num=MARS_BOARD_PIN_LCD_D2, .data3_io_num=MARS_BOARD_PIN_LCD_D3,
        .data4_io_num=-1,.data5_io_num=-1,.data6_io_num=-1,.data7_io_num=-1,
        .max_transfer_sz=W*8*2};
    ESP_RETURN_ON_ERROR(spi_bus_initialize(SPI2_HOST,&spi,SPI_DMA_CH_AUTO),TAG,"QSPI bus");
    spi_device_interface_config_t device_cfg={.mode=3,
        .clock_speed_hz=MARS_BOARD_DISPLAY_CLOCK_HZ,.spics_io_num=MARS_BOARD_PIN_LCD_CS,
        .queue_size=2,.flags=SPI_DEVICE_HALFDUPLEX};
    ESP_RETURN_ON_ERROR(spi_bus_add_device(SPI2_HOST,&device_cfg,&stream_device),TAG,"panel SPI device");
    esp_lcd_panel_io_handle_t io=&owned_io;panel_io=io;
    spd2010_vendor_config_t vendor={.flags.use_qspi_interface=1};
    esp_lcd_panel_dev_config_t panel_cfg={.reset_gpio_num=-1,.rgb_ele_order=LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel=16,.vendor_config=&vendor};
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_spd2010(io,&panel_cfg,&panel),TAG,"SPD2010");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_reset(panel),TAG,"panel reset");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_init(panel),TAG,"panel init");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_disp_on_off(panel,true),TAG,"display on");
    initialized=true;
    ESP_RETURN_ON_ERROR(mars_board_fill_rect(0,0,W,H,0),TAG,"clear panel");
    gpio_set_level(MARS_BOARD_PIN_LCD_BACKLIGHT,1);
    esp_err_t touch = waveshare146_touch_init(bus);
    ESP_LOGI(TAG,"412x412 QSPI ready; app viewport=(%d,%d) %dx%d; touch=%s",
        MARS_BOARD_UI_X,MARS_BOARD_UI_Y,MARS_BOARD_UI_WIDTH,MARS_BOARD_UI_HEIGHT,esp_err_to_name(touch));
    return ESP_OK;
}
esp_err_t mars_board_i2c_bus_get(i2c_master_bus_handle_t *out)
{ if (!out || !bus) return ESP_ERR_INVALID_STATE; *out=bus; return ESP_OK; }
mars_touch_sample_t mars_board_touch_sample(void){return waveshare146_touch_sample();}
bool mars_board_touch_available(void) { return waveshare146_touch_available(); }
bool mars_board_touch_read(uint16_t *x,uint16_t *y) { return waveshare146_touch_read(x,y); }
/* PCM5101 has no control bus; no claim of an I2C codec detection. */
bool mars_board_codec_present(void) { return false; }
esp_err_t mars_board_battery_read(int *percent,int *mv)
{ (void)percent; (void)mv; return ESP_ERR_NOT_SUPPORTED; }
int mars_board_key_read(void)
{
    if (!gpio_get_level(MARS_BOARD_PIN_KEY)) return 3;
    if (!gpio_get_level(MARS_BOARD_PIN_POWER_KEY)) return 2;
    return 0;
}
/* Legacy application ABI: translation only; physical driver stays native. */
esp_err_t box3_board_init(void) { return mars_board_init(); }
esp_err_t box3_i2c_bus_get(i2c_master_bus_handle_t *out) { return mars_board_i2c_bus_get(out); }
bool box3_touch_available(void) { return mars_board_touch_available(); }
int box3_key_read(void) { return mars_board_key_read(); }
bool box3_touch_read(uint16_t *x,uint16_t *y)
{
    uint16_t px,py;
    if (!x || !y || !mars_board_touch_read(&px,&py) || px<MARS_BOARD_UI_X || py<MARS_BOARD_UI_Y
        || px>=MARS_BOARD_UI_X+BOX3_LCD_WIDTH || py>=MARS_BOARD_UI_Y+BOX3_LCD_HEIGHT) return false;
    *x=px-MARS_BOARD_UI_X; *y=py-MARS_BOARD_UI_Y; return true;
}
esp_err_t box3_display_fill_rect(int x,int y,int w,int h,uint16_t c)
{
    if (x<0 || y<0 || w<=0 || h<=0 || w>BOX3_LCD_WIDTH || h>BOX3_LCD_HEIGHT
        || x>BOX3_LCD_WIDTH-w || y>BOX3_LCD_HEIGHT-h) return ESP_ERR_INVALID_ARG;
    return mars_board_fill_rect(x+MARS_BOARD_UI_X,y+MARS_BOARD_UI_Y,w,h,c);
}
esp_err_t box3_display_blit(int x,int y,int w,int h,const uint16_t *pixels)
{
    if (x<0 || y<0 || w<=0 || h<=0 || w>BOX3_LCD_WIDTH || h>BOX3_LCD_HEIGHT
        || x>BOX3_LCD_WIDTH-w || y>BOX3_LCD_HEIGHT-h) return ESP_ERR_INVALID_ARG;
    return mars_board_blit(x+MARS_BOARD_UI_X,y+MARS_BOARD_UI_Y,w,h,pixels);
}
