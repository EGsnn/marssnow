idf_component_register(
    SRCS
        "mars_audio_stream.c"
        "mars_audio_stream_core.c"
        "mars_resample.c"
        "mars_voice_effect.c"
        "mars_audio_stream_selftest.c"
    INCLUDE_DIRS "include"
    REQUIRES
        log
        freertos
        esp_timer
        mars_media
        espressif__esp_audio_codec
)
