#include "mars_desktop.h"

#include "mars_board.h"
#include "esp_check.h"
#include "esp_log.h"
#include "launcher.h"
#include "mars_media.h"
#include "settings_bt.h"
#include "settings_wifi.h"

static const char *TAG = "mars_desktop";

void mars_desktop_run(void)
{
    settings_wifi_init();
    settings_bt_init();
    ESP_ERROR_CHECK(mars_board_init());

    /* XiaoZhi's audio pipeline expects 24 kHz in both directions. */
    const mars_media_cfg_t audio = {
        .input_sample_rate = 24000,
        .output_sample_rate = 24000,
    };
    ESP_ERROR_CHECK(mars_media_init(&audio));
    ESP_LOGI(TAG, "Mars desktop ready; shared Wi-Fi and audio initialized");
    launcher_run();
}
