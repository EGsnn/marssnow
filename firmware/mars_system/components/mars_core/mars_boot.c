/* mars_boot：App 固件引导切换（小智 App 化第 6 步）
 * 分区有效性校验 + 切换引导分区 + 重启。
 * 依赖分区表（2026-10-03 第 7 步新布局）：
 *   factory=火星出厂兜底、ota_0/ota_1=火星 A/B、ota_2/ota_3=小智 A/B。 */
#include "mars_boot.h"

#include <string.h>

#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_system.h"

#include "mars_event.h"

#define ESP_IMAGE_MAGIC 0xE9 /* esp_image_header_t magic */

static const char *TAG = "mars_boot";

static bool partition_valid(const esp_partition_t *p)
{
    if (p == NULL) {
        return false;
    }
    uint8_t magic = 0;
    if (esp_partition_read(p, 0, &magic, 1) != ESP_OK) {
        return false;
    }
    return magic == ESP_IMAGE_MAGIC;
}

static const esp_partition_t *find_running_system(void)
{
    /* 本系统：当前运行分区（factory 或火星 ota_0/ota_1） */
    return esp_ota_get_running_partition();
}

static const esp_partition_t *find_xiaozhi(void)
{
    /* 小智 A/B：优先 ota_2，其次 ota_3（升级换边后可引导） */
    const esp_partition_t *p =
        esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_OTA_2, "ota_2");
    if (partition_valid(p)) {
        return p;
    }
    p = esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_OTA_3, "ota_3");
    return partition_valid(p) ? p : NULL;
}

static const esp_partition_t *find_target(const char *app_id)
{
    if (app_id == NULL) {
        return NULL;
    }
    if (strcmp(app_id, "xiaozhi") == 0) {
        return find_xiaozhi();
    }
    if (strcmp(app_id, "box") == 0 || strcmp(app_id, "factory") == 0) {
        return find_running_system();
    }
    return NULL;
}

bool mars_boot_check(const char *app_id)
{
    return partition_valid(find_target(app_id));
}

esp_err_t mars_boot_switch(const char *app_id)
{
    const esp_partition_t *target = find_target(app_id);
    if (!partition_valid(target)) {
        ESP_LOGW(TAG, "no valid boot target for '%s'", app_id ? app_id : "(null)");
        return ESP_ERR_NOT_FOUND;
    }
    ESP_LOGI(TAG, "switching to '%s' partition %s, rebooting...", app_id, target->label);
    mars_event_post(MARS_EVENT_BOOT_SWITCH, app_id, strlen(app_id) + 1);
    const esp_err_t err = esp_ota_set_boot_partition(target);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "set boot partition failed: %s", esp_err_to_name(err));
        return err;
    }
    esp_restart();
    return ESP_OK; /* 不应到达 */
}
