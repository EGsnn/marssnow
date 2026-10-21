/* SPD2010 protocol follows Waveshare's locked official Touch_SPD2010 example.
 * Bounded packet lengths/draining; every I2C failure returns no contact. */
#include "mars_board.h"
#include "spd2010_touch_packet.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
static i2c_master_dev_handle_t device;
static bool available;
static esp_err_t read_reg(uint16_t reg,uint8_t *data,size_t length)
{
    uint8_t address[]={reg>>8,reg&255};
    return i2c_master_transmit_receive(device,address,2,data,length,25);
}
static esp_err_t command(uint16_t reg,uint8_t value)
{
    uint8_t data[]={reg>>8,reg&255,value,0};
    esp_err_t err=i2c_master_transmit(device,data,4,25);
    esp_rom_delay_us(200);
    return err;
}
bool waveshare146_touch_available(void) { return available; }
/* Keep sparse reports distinct from release. Fast UI polling must not split
 * one finger movement into clicks merely because a poll has no new packet. */
static mars_touch_sample_t delivered;
static mars_touch_sample_t events[8];
static unsigned head,tail;
static void enqueue(mars_touch_sample_t sample)
{
 if(tail-head==8){head=tail=0;sample.phase=MARS_TOUCH_CANCEL;sample.down=false;}
 events[tail++%8]=sample;
}
static void report(const uint8_t *packet,unsigned length,int64_t now)
{
 uint16_t x,y;bool pressed;
 mars_touch_sample_t last=head!=tail?events[(tail-1)%8]:delivered;
 if(length==4||(length>=7&&packet[4]==0xf6)){
  ESP_LOGI("waveshare_touch","release packet bytes=%u",length);
  if(last.down)enqueue((mars_touch_sample_t){.phase=MARS_TOUCH_UP,.x=last.x,.y=last.y,.time_ms=now});
  return;
 }
 if(spd2010_contact_report(packet,length,MARS_BOARD_DISPLAY_WIDTH,MARS_BOARD_DISPLAY_HEIGHT,&x,&y,&pressed)){
  unsigned fingers=(length-4)/6;
  enqueue((mars_touch_sample_t){.phase=fingers>1?MARS_TOUCH_CANCEL:pressed?MARS_TOUCH_POINT:MARS_TOUCH_UP,
   .down=pressed&&fingers==1,.fingers=fingers,.x=x,.y=y,.time_ms=now});
 }
}
static mars_touch_sample_t cancel_sample(int64_t now)
{
 head=tail=0;delivered.phase=MARS_TOUCH_CANCEL;delivered.down=false;delivered.time_ms=now;return delivered;
}
mars_touch_sample_t waveshare146_touch_sample(void)
{
 int64_t now=esp_timer_get_time()/1000;
 if(head!=tail){delivered=events[head++%8];return delivered;}
 if(!available)return cancel_sample(now);
 uint8_t status[4];
 if(read_reg(0x2000,status,4)!=ESP_OK)return cancel_sample(now);
 if(status[1]&0x40){if(command(0x0200,1)==ESP_OK)command(0x0400,1);return cancel_sample(now);}
 if(status[1]&0x20){if(command(0x5000,0)==ESP_OK&&command(0x4600,0)==ESP_OK)command(0x0200,1);return cancel_sample(now);}
 unsigned length=status[2]|(status[3]<<8);
 /* Status-only evidence, never password coordinates or characters. */
 static uint32_t last_status=UINT32_MAX;
 uint32_t current_status=status[0]|((uint32_t)status[1]<<8)|((uint32_t)length<<16);
 if(current_status!=last_status){
  if(delivered.down||length)ESP_LOGI("waveshare_touch","status low=%02x high=%02x bytes=%u",status[0],status[1],length);
  last_status=current_status;
 }
 if(!length||!(status[0]&3)){
  if(((status[1]&8)||(status[0]&8))&&command(0x0200,1)!=ESP_OK)return cancel_sample(now);
  /* In point mode this panel also ends contacts by returning to CPU-running
   * idle, without a zero-pressure packet. Debounce that explicit idle state;
   * a single poll with no fresh packet must still retain contact. */
  if(delivered.down&&(status[1]&8)&&!(status[0]&3)&&!length&&now-delivered.time_ms>=100){
   ESP_LOGI("waveshare_touch","release=idle gap_ms=%ld",(long)(now-delivered.time_ms));
   delivered.phase=MARS_TOUCH_UP;delivered.down=false;delivered.time_ms=now;return delivered;
  }
  if(delivered.down&&now-delivered.time_ms>2000)return cancel_sample(now);
  mars_touch_sample_t idle=delivered;idle.phase=MARS_TOUCH_NONE;return idle;
 }
 uint8_t packet[64];
 if(length<4||length>sizeof(packet)||read_reg(0x0003,packet,length)!=ESP_OK){command(0x0200,1);return cancel_sample(now);}
 report(packet,length,now);
 bool complete=false;
 for(int i=0;i<4;++i){
  uint8_t rest[8];if(read_reg(0xfc02,rest,8)!=ESP_OK)break;
  if(rest[5]==0x82){complete=true;break;}
  unsigned remain=rest[2]|(rest[3]<<8);
  if(rest[5]!=0||remain<4||remain>sizeof(packet)||read_reg(0x0003,packet,remain)!=ESP_OK)break;
  report(packet,remain,now);
 }
 if(command(0x0200,1)!=ESP_OK||!complete)return cancel_sample(now);
 if(head!=tail){delivered=events[head++%8];return delivered;}
 mars_touch_sample_t idle=delivered;idle.phase=MARS_TOUCH_NONE;return idle;
}
bool waveshare146_touch_read(uint16_t *x,uint16_t *y)
{
 if(!x||!y)return false;
 mars_touch_sample_t sample=waveshare146_touch_sample();*x=sample.x;*y=sample.y;return sample.down;
}
esp_err_t waveshare146_touch_init(i2c_master_bus_handle_t bus)
{
    i2c_device_config_t cfg={.dev_addr_length=I2C_ADDR_BIT_LEN_7,.device_address=MARS_BOARD_TOUCH_ADDRESS,.scl_speed_hz=400000};
    esp_err_t err=i2c_master_bus_add_device(bus,&cfg,&device);
    if (err!=ESP_OK) return err;
    uint8_t version[18];
    err=read_reg(0x2600,version,sizeof(version));
    if (err!=ESP_OK) return err;
    available=true;
    for (int i=0;i<4;++i) { uint16_t x,y; waveshare146_touch_read(&x,&y); vTaskDelay(pdMS_TO_TICKS(10)); }
    ESP_LOGI("waveshare146","SPD2010 touch responded at 0x53");
    return ESP_OK;
}
