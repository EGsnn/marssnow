if(MARS_BOARD STREQUAL "waveshare-esp32-s3-touch-lcd-1-46")
    set(MARS_AUDIO_BACKEND "mars_audio_pcm5101.c")
else()
    set(MARS_AUDIO_BACKEND "mars_audio_es8311.c")
endif()
idf_component_register(
    SRCS
        "mars_media.c"
        ${MARS_AUDIO_BACKEND}
        "output_owner.c"
    INCLUDE_DIRS "include"
    REQUIRES driver esp_driver_i2s esp_driver_i2c espressif__esp_codec_dev mars_bsp mars_core freertos
)
