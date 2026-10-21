idf_component_register(
    SRCS "src/mars_native_ui.c" "src/mars_services.c" "src/mars_runtime.c" "src/mars_app_preferences.c" "src/mars_emote_player.c" "src/mars_ws.c" "src/mars_ws_config.c" "src/mars_ws_reassembly.c"
    INCLUDE_DIRS "include"
    REQUIRES driver esp_timer log heap freertos mars_media mars_net mars_audio_stream mars_common
             mars_clock mars_emote_gfx espressif__esp_websocket_client nvs_flash mbedtls esp_hw_support
)
