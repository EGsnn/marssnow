#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
esp_err_t mars_audio_hw_init(int input_rate,int output_rate);
esp_err_t mars_audio_hw_state(bool output,bool input,int volume);
bool mars_audio_hw_active(void);
esp_err_t mars_audio_hw_volume(int volume);
esp_err_t mars_audio_hw_write(const int16_t *pcm,size_t samples,uint32_t timeout_ms);
esp_err_t mars_audio_hw_read(int16_t *pcm,size_t samples);
void mars_audio_hw_deinit(void);
