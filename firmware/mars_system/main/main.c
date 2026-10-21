#include "mars_board.h"
#include "mars_ui_adapt.h"
#include "launcher.h"
#include "mars_media.h"
#include "mars_clock.h"
#include "mars_app_loader.h"
#include "settings_bt.h"
#include "settings_wifi.h"
#include "ui_runtime.h"

#include "esp_log.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

/* P1：音频自测命令接入现有唯一串口行解析器（ui_runtime_poll）。
 * 不再单独创建 console_task / 提前安装 USB Serial/JTAG 驱动——
 * 整个系统只有 ui_runtime 一个 STDIN/USB-JTAG 消费者。 */
#include "mars_audio_stream.h"
#include "mars_audio_stream_selftest.h"
#include "mars_ws.h"
#include "mars_app_preferences.h"

static const char *TAG = "box_desktop";

void app_main(void)
{
    ESP_LOGI(TAG, "Starting ATK BOX JSON UI runtime v0.5-settings");
    const esp_err_t nvs_result = nvs_flash_init();
    if (nvs_result != ESP_OK) {
        ESP_LOGW(TAG, "NVS unavailable; continuing without saved UI: %s", esp_err_to_name(nvs_result));
    }
    /* 网络/BLE 先初始化（与迁移前一致，BLE 扫描依赖此顺序下充足的内部 RAM）；
     * PSRAM 保留池已调小，display 的 DMA 仍可正常分配 */
    settings_wifi_init();
    settings_bt_init();
    ESP_ERROR_CHECK(mars_board_init());
    const mars_ui_profile_t *ui = mars_ui_profile();
    ESP_LOGI(TAG, "UI profile layout=%s motion=%s target=%u FPS sha=%.12s (legacy viewport retained)",
             ui->layout, ui->motion_tier, (unsigned)mars_ui_motion().fps, ui->sha256);
    /* L1 音频能力：初始化当前板型的音频后端；失败不阻塞桌面。 */
    mars_media_init(NULL);
    /* P1 音频流服务：创建生命周期锁（必须早于任何动态应用/并发调用）。 */
    mars_audio_stream_init();
    if(mars_app_preferences_init()!=0)ESP_LOGW(TAG,"App preferences unavailable");
    mars_ws_init();
    /* Time is a system service. Subscribe before opening any app so Wi-Fi
     * connects trigger synchronization even if the built-in clock is unused.
     * Run cache/NVS initialization on this internal stack, not an app's PSRAM stack. */
    mars_clock_init();
    /* 把 Opus 编解码自测挂到唯一串口行解析器（@AUDIO:SELFTEST）。 */
    ui_runtime_set_selftest_handler(mars_audio_stream_selftest);
    /* 动态应用加载器先初始化；应用注册由 launcher 在内置应用之后完成。 */
    mars_app_loader_init();
    ESP_LOGI(TAG, "Display ready; opening six-app launcher");
    launcher_run();
}
