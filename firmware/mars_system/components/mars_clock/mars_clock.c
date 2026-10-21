#include "mars_clock.h"

#include <stdio.h>
#include <string.h>

#include "cJSON.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_netif_sntp.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mars_event.h"
#include "mars_net.h"
#include "nvs.h"

#define TAG "mars_clock"

#define NVS_NS "clock"
#define TZ_NAME_MAX 40
#define SNTP_SERVER "ntp.aliyun.com" /* 国内可达性好；需要时可改 pool.ntp.org */
#define SNTP_VALID_EPOCH 1700000000LL /* 2023-11-14，早于它视为尚未同步 */
#define SNTP_WAIT_TICKS pdMS_TO_TICKS(15000)
#define TZ_HTTP_TIMEOUT_MS 6000
#define SYNC_STACK 4096

static mars_clock_state_t s_state = MARS_CLOCK_NO_NET;
static int s_tz_offset_sec;
static char s_tz_name[TZ_NAME_MAX] = "UTC";
static bool s_started;

/* ── 时区状态 ── */

static void set_tz(const char *name, int offset_sec)
{
    strlcpy(s_tz_name, (name && name[0]) ? name : "UTC", sizeof(s_tz_name));
    s_tz_offset_sec = offset_sec;
}

/* ── NVS 缓存 ── */

static void save_tz_cache(void)
{
    nvs_handle_t handle;
    if (nvs_open(NVS_NS, NVS_READWRITE, &handle) != ESP_OK) return;
    nvs_set_i32(handle, "tz_offset", s_tz_offset_sec);
    nvs_set_str(handle, "tz_name", s_tz_name);
    nvs_commit(handle);
    nvs_close(handle);
}

static void load_tz_cache(void)
{
    nvs_handle_t handle;
    if (nvs_open(NVS_NS, NVS_READONLY, &handle) != ESP_OK) return;
    int32_t off = 0;
    if (nvs_get_i32(handle, "tz_offset", &off) == ESP_OK) {
        s_tz_offset_sec = (int)off;
    }
    size_t len = sizeof(s_tz_name);
    if (nvs_get_str(handle, "tz_name", s_tz_name, &len) != ESP_OK || !s_tz_name[0]) {
        strlcpy(s_tz_name, "UTC", sizeof(s_tz_name));
    }
    nvs_close(handle);
}

/* ── HTTP 获取 IP 时区 ── */

typedef struct {
    char buf[512];
    size_t len;
} http_ctx_t;

static esp_err_t http_event_handler(esp_http_client_event_t *evt)
{
    http_ctx_t *ctx = (http_ctx_t *)evt->user_data;
    if (evt->event_id == HTTP_EVENT_ON_DATA) {
        const size_t room = sizeof(ctx->buf) - ctx->len - 1;
        if (room > 0) {
            const size_t n = evt->data_len < room ? evt->data_len : room;
            memcpy(ctx->buf + ctx->len, evt->data, n);
            ctx->len += n;
        }
    } else if (evt->event_id == HTTP_EVENT_ON_FINISH) {
        ctx->buf[ctx->len] = 0;
    }
    return ESP_OK;
}

static bool http_get(const char *url, char *out, size_t out_size)
{
    http_ctx_t ctx = {0};
    const esp_http_client_config_t cfg = {
        .url = url,
        .event_handler = http_event_handler,
        .user_data = &ctx,
        .timeout_ms = TZ_HTTP_TIMEOUT_MS,
        .buffer_size = 1024,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) return false;
    const esp_err_t err = esp_http_client_perform(client);
    esp_http_client_cleanup(client);
    if (err != ESP_OK || ctx.len == 0) return false;
    strlcpy(out, ctx.buf, out_size);
    return true;
}

/* "+08:00" / "-05:30" → 秒 */
static int parse_utc_offset(const char *text)
{
    if (!text || !*text) return 0;
    int sign = 1;
    const char *p = text;
    if (*p == '-') { sign = -1; ++p; }
    else if (*p == '+') ++p;
    int h = 0, m = 0;
    if (sscanf(p, "%d:%d", &h, &m) < 1) return 0;
    if (h > 14 || m > 59) return 0;
    return sign * (h * 3600 + m * 60);
}

/* 在线获取 IP 所在时区；成功返回 true（失败保留缓存值） */
static bool fetch_tz_from_ip(void)
{
    char body[512];

    /* 主源 ip-api.com：明文 HTTP、无需 key，返回 timezone + offset（秒） */
    if (http_get("http://ip-api.com/json/?fields=status,timezone,offset",
                 body, sizeof(body))) {
        cJSON *root = cJSON_Parse(body);
        if (root) {
            const cJSON *status = cJSON_GetObjectItem(root, "status");
            const cJSON *tz = cJSON_GetObjectItem(root, "timezone");
            const cJSON *off = cJSON_GetObjectItem(root, "offset");
            if (status && cJSON_IsString(status) &&
                strcmp(status->valuestring, "success") == 0 &&
                tz && cJSON_IsString(tz) && off && cJSON_IsNumber(off)) {
                set_tz(tz->valuestring, (int)off->valuedouble);
                cJSON_Delete(root);
                return true;
            }
            cJSON_Delete(root);
        }
    }

    /* 备用源 worldtimeapi.org：http 自动跳 https，返回 timezone + utc_offset */
    if (http_get("http://worldtimeapi.org/api/ip", body, sizeof(body))) {
        cJSON *root = cJSON_Parse(body);
        if (root) {
            const cJSON *tz = cJSON_GetObjectItem(root, "timezone");
            const cJSON *off = cJSON_GetObjectItem(root, "utc_offset");
            if (tz && cJSON_IsString(tz) && off && cJSON_IsString(off)) {
                set_tz(tz->valuestring, parse_utc_offset(off->valuestring));
                cJSON_Delete(root);
                return true;
            }
            cJSON_Delete(root);
        }
    }
    return false;
}

/* ── 后台同步任务 ── */

static void sync_task(void *arg)
{
    (void)arg;

    const esp_sntp_config_t cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG(SNTP_SERVER);
    if (esp_netif_sntp_init(&cfg) != ESP_OK) {
        ESP_LOGW(TAG, "SNTP init failed");
        s_state = MARS_CLOCK_FAILED;
        s_started = false;
        vTaskDelete(NULL);
        return;
    }
    if (esp_netif_sntp_sync_wait(SNTP_WAIT_TICKS) != ESP_OK) {
        ESP_LOGW(TAG, "SNTP sync timeout");
        s_state = MARS_CLOCK_FAILED;
        s_started = false;
        vTaskDelete(NULL);
        return;
    }
    if (time(NULL) <= SNTP_VALID_EPOCH) {
        ESP_LOGW(TAG, "SNTP returned invalid epoch");
        s_state = MARS_CLOCK_FAILED;
        s_started = false;
        vTaskDelete(NULL);
        return;
    }

    /* 时区定位：优先在线获取，失败沿用缓存 */
    bool live = false;
    if (mars_net_state() == MARS_WIFI_CONNECTED) {
        live = fetch_tz_from_ip();
    }
    save_tz_cache();
    s_state = MARS_CLOCK_SYNCED;
    ESP_LOGI(TAG, "clock synced: %s UTC%+d (%s)",
             s_tz_name, s_tz_offset_sec / 3600, live ? "live" : "cached");
    s_started = false;
    vTaskDelete(NULL);
}

/* ── 生命周期 ── */

static void on_wifi_event(const mars_event_t *event, void *ctx)
{
    (void)ctx;
    if (event->id == MARS_EVENT_WIFI_CONNECTED) {
        mars_clock_start();
    } else if (event->id == MARS_EVENT_WIFI_DISCONNECTED) {
        if (!s_started) s_state = MARS_CLOCK_NO_NET;
    }
}

void mars_clock_init(void)
{
    static bool ready;
    if (ready) return;
    ready = true;
    load_tz_cache();
    mars_event_subscribe(MARS_EVENT_WIFI_CONNECTED, on_wifi_event, NULL);
    mars_event_subscribe(MARS_EVENT_WIFI_DISCONNECTED, on_wifi_event, NULL);
    if (mars_net_state() == MARS_WIFI_CONNECTED) {
        mars_clock_start();
    }
}

void mars_clock_start(void)
{
    if (s_started) return;
    s_started = true;
    s_state = MARS_CLOCK_SYNCING;
    if (xTaskCreate(sync_task, "mars_clock", SYNC_STACK, NULL, 5, NULL) != pdPASS) {
        s_started = false;
        s_state = MARS_CLOCK_FAILED;
    }
}

/* ── 查询 ── */

mars_clock_state_t mars_clock_state(void)
{
    return s_state;
}

bool mars_clock_get_local_time(struct tm *out)
{
    if (!out) return false;
    const time_t now = time(NULL);
    if (now <= SNTP_VALID_EPOCH) return false;
    const time_t local = now + s_tz_offset_sec;
    return gmtime_r(&local, out) != NULL;
}

int mars_clock_tz_offset_sec(void)
{
    return s_tz_offset_sec;
}

const char *mars_clock_tz_name(void)
{
    return s_tz_name;
}
