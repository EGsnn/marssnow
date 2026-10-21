#include "mars_ota.h"

#include <string.h>

#include "cJSON.h"
#include "esp_app_desc.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "mbedtls/md.h"
#include "mars_event.h"

#define TAG "mars_ota"
#define MANIFEST_MAX 8192

#ifndef CONFIG_MARS_OTA_MANIFEST_URL
#define CONFIG_MARS_OTA_MANIFEST_URL NULL
#endif

static const char *s_manifest_url = CONFIG_MARS_OTA_MANIFEST_URL;
static mars_ota_state_t s_state = MARS_OTA_IDLE;
static char s_error[128];
static char s_app_id[32];
static char s_latest_version[16];
static bool s_has_update;
static char s_update_url[256];
static char s_target_part_label[16];
static char s_sha256_hex[65];
static size_t s_target_size;

static void set_error(const char *msg)
{
    strlcpy(s_error, msg, sizeof(s_error));
    ESP_LOGE(TAG, "%s", msg);
}

static void set_state(mars_ota_state_t st)
{
    if (s_state == st) return;
    s_state = st;
    mars_event_post(MARS_EVENT_OTA_STATE_CHANGED, &s_state, sizeof(s_state));
}

/* ── 版本比较（数字段） ── */

static int version_compare(const char *a, const char *b)
{
    if (!a) a = "";
    if (!b) b = "";
    while (*a || *b) {
        int va = 0;
        int vb = 0;
        while (*a && *a != '.') {
            if (*a >= '0' && *a <= '9') va = va * 10 + (*a - '0');
            ++a;
        }
        while (*b && *b != '.') {
            if (*b >= '0' && *b <= '9') vb = vb * 10 + (*b - '0');
            ++b;
        }
        if (va != vb) return va < vb ? -1 : 1;
        if (*a) ++a;
        if (*b) ++b;
    }
    return 0;
}

/* ── SHA256 校验 ── */

static int hex_val(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static bool sha256_match(const uint8_t *digest, const char *hex)
{
    if (!hex || strlen(hex) != 64) return false;
    for (int i = 0; i < 32; ++i) {
        const int hi = hex_val(hex[i * 2]);
        const int lo = hex_val(hex[i * 2 + 1]);
        if (hi < 0 || lo < 0 || ((hi << 4) | lo) != digest[i]) return false;
    }
    return true;
}

/* ── HTTP 小体量下载（manifest） ── */

typedef struct {
    char *buf;
    size_t len;
    size_t cap;
    esp_err_t err;
} http_buf_t;

static esp_err_t manifest_event(esp_http_client_event_t *evt)
{
    http_buf_t *b = (http_buf_t *)evt->user_data;
    if (evt->event_id == HTTP_EVENT_ON_DATA) {
        if (b->len + evt->data_len + 1 > b->cap) return ESP_FAIL;
        memcpy(b->buf + b->len, evt->data, evt->data_len);
        b->len += evt->data_len;
        b->buf[b->len] = '\0';
    } else if (evt->event_id == HTTP_EVENT_ERROR) {
        b->err = ESP_FAIL;
    }
    return ESP_OK;
}

static bool http_get_small(const char *url, char *buf, size_t cap)
{
    http_buf_t b = {.buf = buf, .cap = cap, .err = ESP_OK};
    esp_http_client_config_t cfg = {
        .url = url,
        .method = HTTP_METHOD_GET,
        .timeout_ms = 10000,
        .event_handler = manifest_event,
        .user_data = &b,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (client == NULL) return false;
    const esp_err_t err = esp_http_client_perform(client);
    esp_http_client_cleanup(client);
    return err == ESP_OK && b.err == ESP_OK && b.len > 0;
}

/* ── 流式下载写 OTA 分区 ── */

typedef struct {
    esp_ota_handle_t handle;
    mbedtls_md_context_t md;
    size_t target_size;
    size_t received;
    void (*progress)(int percent, void *ctx);
    void *ctx;
    esp_err_t err;
} ota_dl_t;

static esp_err_t firmware_event(esp_http_client_event_t *evt)
{
    ota_dl_t *d = (ota_dl_t *)evt->user_data;
    if (evt->event_id == HTTP_EVENT_ON_DATA) {
        const int len = evt->data_len;
        if (len <= 0 || esp_ota_write(d->handle, evt->data, (size_t)len) != ESP_OK) {
            d->err = ESP_FAIL;
            return ESP_FAIL;
        }
        mbedtls_md_update(&d->md, evt->data, (size_t)len);
        d->received += (size_t)len;
        if (d->progress != NULL && d->target_size > 0) {
            int percent = (int)(d->received * 100 / d->target_size);
            if (percent > 100) percent = 100;
            d->progress(percent, d->ctx);
            mars_event_post(MARS_EVENT_OTA_PROGRESS, &percent, sizeof(percent));
        }
    } else if (evt->event_id == HTTP_EVENT_ERROR) {
        d->err = ESP_FAIL;
    }
    return ESP_OK;
}

bool mars_ota_check(const char *app_id)
{
    if (app_id == NULL || app_id[0] == '\0') {
        set_error("app_id empty");
        return false;
    }
    if (s_manifest_url == NULL || s_manifest_url[0] == '\0') {
        set_error("manifest url not set");
        set_state(MARS_OTA_FAILED);
        return false;
    }
    strlcpy(s_app_id, app_id, sizeof(s_app_id));
    set_state(MARS_OTA_CHECKING);

    char *json = malloc(MANIFEST_MAX);
    if (json == NULL) {
        set_error("manifest malloc failed");
        set_state(MARS_OTA_FAILED);
        return false;
    }
    const bool ok = http_get_small(s_manifest_url, json, MANIFEST_MAX);
    if (!ok) {
        free(json);
        set_error("manifest download failed");
        set_state(MARS_OTA_FAILED);
        return false;
    }
    cJSON *root = cJSON_Parse(json);
    free(json);
    if (root == NULL) {
        set_error("manifest parse failed");
        set_state(MARS_OTA_FAILED);
        return false;
    }
    cJSON *app = cJSON_GetObjectItem(root, app_id);
    if (app == NULL) {
        cJSON_Delete(root);
        set_error("app not in manifest");
        set_state(MARS_OTA_FAILED);
        return false;
    }

    s_has_update = false;
    s_latest_version[0] = '\0';
    s_update_url[0] = '\0';
    s_target_part_label[0] = '\0';
    s_sha256_hex[0] = '\0';
    s_target_size = 0;

    const cJSON *ver = cJSON_GetObjectItem(app, "version");
    const cJSON *url = cJSON_GetObjectItem(app, "url");
    const cJSON *part = cJSON_GetObjectItem(app, "part");
    const cJSON *sha = cJSON_GetObjectItem(app, "sha256");
    const cJSON *size = cJSON_GetObjectItem(app, "size");
    if (ver != NULL && ver->valuestring != NULL && ver->valuestring[0]) {
        strlcpy(s_latest_version, ver->valuestring, sizeof(s_latest_version));
    }
    if (url != NULL && url->valuestring != NULL) {
        strlcpy(s_update_url, url->valuestring, sizeof(s_update_url));
    }
    if (part != NULL && part->valuestring != NULL) {
        strlcpy(s_target_part_label, part->valuestring, sizeof(s_target_part_label));
    }
    if (sha != NULL && sha->valuestring != NULL) {
        strlcpy(s_sha256_hex, sha->valuestring, sizeof(s_sha256_hex));
    }
    if (size != NULL && size->valuedouble > 0) {
        s_target_size = (size_t)size->valuedouble;
    }
    cJSON_Delete(root);

    const esp_app_desc_t *desc = esp_app_get_description();
    const char *running = (desc != NULL && desc->version[0]) ? desc->version : "0.0.0";
    s_has_update = version_compare(running, s_latest_version) < 0;
    ESP_LOGI(TAG, "app=%s running=%s latest=%s update=%d", s_app_id, running,
             s_latest_version, s_has_update);
    set_state(s_has_update ? MARS_OTA_READY : MARS_OTA_IDLE);
    return s_has_update;
}

mars_ota_state_t mars_ota_state(void)
{
    return s_state;
}

const char *mars_ota_error(void)
{
    return s_error;
}

const char *mars_ota_latest_version(void)
{
    return s_latest_version;
}

bool mars_ota_has_update(void)
{
    return s_has_update;
}

const char *mars_ota_update_url(void)
{
    return s_update_url;
}

const char *mars_ota_target_partition(void)
{
    return s_target_part_label;
}

bool mars_ota_update(void (*progress)(int percent, void *ctx), void *ctx)
{
    if (s_state != MARS_OTA_READY || s_update_url[0] == '\0') {
        set_error("no update ready");
        return false;
    }
    const esp_partition_t *running = esp_ota_get_running_partition();
    const esp_partition_t *target = s_target_part_label[0]
        ? esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_ANY,
                                   s_target_part_label)
        : esp_ota_get_next_update_partition(running);
    if (target == NULL) {
        set_error("target partition not found");
        set_state(MARS_OTA_FAILED);
        return false;
    }
    if (target->subtype < ESP_PARTITION_SUBTYPE_APP_OTA_MIN ||
        target->subtype > ESP_PARTITION_SUBTYPE_APP_OTA_MAX) {
        set_error("target partition is not an OTA slot");
        set_state(MARS_OTA_FAILED);
        return false;
    }
    if (target == running) {
        set_error("target partition is running");
        set_state(MARS_OTA_FAILED);
        return false;
    }

    esp_ota_handle_t handle = 0;
    if (esp_ota_begin(target, OTA_SIZE_UNKNOWN, &handle) != ESP_OK) {
        set_error("esp_ota_begin failed");
        set_state(MARS_OTA_FAILED);
        return false;
    }
    set_state(MARS_OTA_UPDATING);

    ota_dl_t dl = {
        .handle = handle,
        .target_size = s_target_size,
        .progress = progress,
        .ctx = ctx,
        .err = ESP_OK,
    };
    mbedtls_md_init(&dl.md);
    if (mbedtls_md_setup(&dl.md, mbedtls_md_info_from_type(MBEDTLS_MD_SHA256), 0) != 0) {
        esp_ota_abort(handle);
        set_error("sha256 setup failed");
        set_state(MARS_OTA_FAILED);
        return false;
    }
    mbedtls_md_starts(&dl.md);

    esp_http_client_config_t cfg = {
        .url = s_update_url,
        .method = HTTP_METHOD_GET,
        .timeout_ms = 30000,
        .event_handler = firmware_event,
        .user_data = &dl,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    const esp_err_t err = client ? esp_http_client_perform(client) : ESP_FAIL;
    if (client) esp_http_client_cleanup(client);

    uint8_t digest[32];
    mbedtls_md_finish(&dl.md, digest);
    mbedtls_md_free(&dl.md);

    if (err != ESP_OK || dl.err != ESP_OK) {
        esp_ota_abort(handle);
        set_error("firmware download failed");
        set_state(MARS_OTA_FAILED);
        return false;
    }
    if (!sha256_match(digest, s_sha256_hex)) {
        esp_ota_abort(handle);
        set_error("sha256 mismatch");
        set_state(MARS_OTA_FAILED);
        return false;
    }
    if (esp_ota_end(handle) != ESP_OK) {
        set_error("esp_ota_end failed");
        set_state(MARS_OTA_FAILED);
        return false;
    }
    if (esp_ota_set_boot_partition(target) != ESP_OK) {
        set_error("set boot partition failed");
        set_state(MARS_OTA_FAILED);
        return false;
    }
    ESP_LOGI(TAG, "update written to %s, reboot to apply", target->label);
    set_state(MARS_OTA_DONE);
    return true;
}

void mars_ota_confirm(void)
{
    esp_ota_mark_app_valid_cancel_rollback();
    ESP_LOGI(TAG, "new firmware confirmed");
}

void mars_ota_rollback(void)
{
    esp_ota_mark_app_invalid_rollback();
    ESP_LOGW(TAG, "rollback: rebooting to previous firmware");
    esp_restart();
}

void mars_ota_set_manifest_url(const char *url)
{
    s_manifest_url = url;
}

void mars_ota_init(void)
{
    /* 懒初始化：状态即初始值，无额外资源 */
}
