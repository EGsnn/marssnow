#include "mars_net.h"

#include <string.h>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mars_event.h"
#include "nvs.h"

#define WIFI_MAX_APS 32

static const char *TAG = "mars_net";

typedef struct {
    char ssid[33];
    int8_t rssi;
    bool secure;
} ap_entry_t;

static ap_entry_t s_aps[WIFI_MAX_APS];
static int s_ap_count;
static mars_wifi_state_t s_state = MARS_WIFI_NONE;
static char s_connected_ssid[33];
static esp_ip4_addr_t s_ip;
static bool s_ready;

static char s_saved_ssid[33];
static char s_saved_pass[65];
static bool s_saved_valid;
static bool s_pending_save,s_credentials_saved;
static char s_pending_ssid[33],s_pending_pass[65];
static esp_netif_t *s_ap_if;

static uint32_t s_scan_gen;
static uint32_t s_scan_done_gen;

static bool s_in_provision;
static char s_provision_ssid[33];
static esp_timer_handle_t s_retry_timer;
static uint32_t s_retry_delay_ms = 1000;

static void set_state(mars_wifi_state_t state);
static void schedule_retry(void);

static void retry_callback(void *arg)
{
    (void)arg;
    if (!s_ready || !s_saved_valid || s_in_provision || s_state == MARS_WIFI_CONNECTED) return;
    esp_err_t err = esp_wifi_connect();
    if (err == ESP_OK) {
        set_state(MARS_WIFI_CONNECTING);
    } else {
        ESP_LOGW(TAG, "Wi-Fi retry start failed: %s", esp_err_to_name(err));
        schedule_retry();
    }
}

static void schedule_retry(void)
{
    if (!s_retry_timer || !s_saved_valid || s_in_provision) return;
    const uint32_t delay_ms = s_retry_delay_ms;
    if (s_retry_delay_ms < 30000) {
        s_retry_delay_ms = delay_ms * 2 > 30000 ? 30000 : delay_ms * 2;
    }
    esp_timer_stop(s_retry_timer);
    ESP_LOGI(TAG, "Wi-Fi retry in %u ms", (unsigned)delay_ms);
    ESP_ERROR_CHECK(esp_timer_start_once(s_retry_timer, (uint64_t)delay_ms * 1000));
}

static void set_state(mars_wifi_state_t state)
{
    if (s_state == state) return;
    s_state = state;
    ESP_LOGI(TAG, "Wi-Fi state=%d", (int)state);
    mars_event_post(MARS_EVENT_WIFI_STATE_CHANGED, &s_state, sizeof(s_state));
}

static bool save_credentials(const char *ssid, const char *password)
{
    nvs_handle_t handle;
    if (nvs_open("settings", NVS_READWRITE, &handle) != ESP_OK) return false;
    esp_err_t err=nvs_set_str(handle,"wifi_ssid",ssid?ssid:"");
    if(err==ESP_OK)err=nvs_set_str(handle,"wifi_pass",password?password:"");
    if(err==ESP_OK)err=nvs_commit(handle);
    nvs_close(handle);return err==ESP_OK;
}

static void load_credentials(void)
{
    nvs_handle_t handle;
    s_saved_valid = false;
    if (nvs_open("settings", NVS_READONLY, &handle) != ESP_OK) return;
    size_t len = sizeof(s_saved_ssid);
    if (nvs_get_str(handle, "wifi_ssid", s_saved_ssid, &len) == ESP_OK && len > 1) {
        len = sizeof(s_saved_pass);
        if (nvs_get_str(handle, "wifi_pass", s_saved_pass, &len) == ESP_OK) {
            s_saved_valid = true;s_credentials_saved=true;
        }
    }
    nvs_close(handle);
}

static void handle_disconnect(int reason)
{
    if(s_in_provision){
        if(s_state==MARS_WIFI_CONNECTING||s_state==MARS_WIFI_CONNECTED)set_state(MARS_WIFI_FAILED);
        return;
    }
    ESP_LOGW(TAG, "Wi-Fi disconnected (reason=%d)", reason);
    if (s_state == MARS_WIFI_CONNECTED || s_state == MARS_WIFI_CONNECTING) {
        set_state(MARS_WIFI_FAILED);
        mars_event_post(MARS_EVENT_WIFI_DISCONNECTED, &reason, sizeof(reason));
        /* 认证超时或暂时找不到 AP 时延迟重试；密码错误、安全模式不兼容需重新配网。 */
        if (reason != WIFI_REASON_AUTH_FAIL &&
            reason != WIFI_REASON_NO_AP_FOUND_W_COMPATIBLE_SECURITY) schedule_retry();
    }
}

static void wifi_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    if (base == WIFI_EVENT) {
        switch (id) {
        case WIFI_EVENT_STA_START:
            ESP_LOGI(TAG, "Wi-Fi STA started (saved=%d)", s_saved_valid);
            if (s_saved_valid) {
                wifi_config_t cfg = {0};
                memcpy(cfg.sta.ssid,s_saved_ssid,strlen(s_saved_ssid));
                if (s_saved_pass[0]) {
                    memcpy(cfg.sta.password,s_saved_pass,strlen(s_saved_pass));
                    cfg.sta.sae_pwe_h2e = WPA3_SAE_PWE_BOTH;
                } else {
                    cfg.sta.threshold.authmode = WIFI_AUTH_OPEN;
                }
                esp_err_t err = esp_wifi_set_config(WIFI_IF_STA, &cfg);
                if (err == ESP_OK) err = esp_wifi_connect();
                if (err == ESP_OK) {
                    strlcpy(s_connected_ssid, s_saved_ssid, sizeof(s_connected_ssid));
                    set_state(MARS_WIFI_CONNECTING);
                } else {
                    ESP_LOGW(TAG, "Wi-Fi auto-connect failed: %s", esp_err_to_name(err));
                    set_state(MARS_WIFI_FAILED);
                }
            }
            break;
        case WIFI_EVENT_SCAN_DONE: {
            uint16_t count = 0;
            esp_wifi_scan_get_ap_num(&count);
            if (count > WIFI_MAX_APS) count = WIFI_MAX_APS;
            wifi_ap_record_t *records = calloc(count ? count : 1, sizeof(wifi_ap_record_t));
            if (records && esp_wifi_scan_get_ap_records(&count, records) == ESP_OK) {
                s_ap_count = 0;
                for (uint16_t i = 0; i < count; ++i) {
                    if (!records[i].ssid[0]) continue;
                    if (s_ap_count >= WIFI_MAX_APS) break;
                    strlcpy(s_aps[s_ap_count].ssid, (const char *)records[i].ssid,
                            sizeof(s_aps[s_ap_count].ssid));
                    s_aps[s_ap_count].rssi = records[i].rssi;
                    s_aps[s_ap_count].secure = records[i].authmode != WIFI_AUTH_OPEN;
                    ++s_ap_count;
                }
            }
            free(records);
            s_scan_done_gen = s_scan_gen;
            mars_event_post(MARS_EVENT_WIFI_SCAN_DONE, &s_ap_count, sizeof(s_ap_count));
            break;
        }
        case WIFI_EVENT_STA_DISCONNECTED: {
            wifi_event_sta_disconnected_t *disc = data;
            handle_disconnect(disc ? disc->reason : WIFI_REASON_UNSPECIFIED);
            break;
        }
        default:
            break;
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        if (s_retry_timer) esp_timer_stop(s_retry_timer);
        s_retry_delay_ms = 1000;
        if(s_pending_save){
            wifi_ap_record_t ap;
            if(esp_wifi_sta_get_ap_info(&ap)!=ESP_OK||strncmp((const char *)ap.ssid,s_pending_ssid,32))return;
            strlcpy(s_saved_ssid,s_pending_ssid,sizeof(s_saved_ssid));strlcpy(s_saved_pass,s_pending_pass,sizeof(s_saved_pass));
            s_saved_valid=true;s_credentials_saved=save_credentials(s_saved_ssid,s_saved_pass);s_pending_save=false;memset(s_pending_pass,0,sizeof(s_pending_pass));
        }
        ESP_LOGI(TAG, "Wi-Fi got IP");
        ip_event_got_ip_t *evt = data;
        if (evt) s_ip = evt->ip_info.ip;
        set_state(MARS_WIFI_CONNECTED);
        mars_event_post(MARS_EVENT_WIFI_CONNECTED, &s_ip, sizeof(s_ip));
    }
}

void mars_net_init(void)
{
    if (s_ready) return;
    load_credentials();
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
#if CONFIG_IDF_TARGET_ESP32C3
    cfg.static_rx_buf_num=4;cfg.dynamic_rx_buf_num=8;cfg.tx_buf_type=1;cfg.dynamic_tx_buf_num=8;
    cfg.rx_ba_win=4;
#endif
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
#if CONFIG_IDF_TARGET_ESP32C3
    /* Candidate credentials remain RAM-only until GOT_IP commits settings. */
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
#endif
    const esp_timer_create_args_t retry_args = {
        .callback = retry_callback,
        .name = "mars_wifi_retry",
    };
    ESP_ERROR_CHECK(esp_timer_create(&retry_args, &s_retry_timer));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                               wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                               wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_wifi_start());
    s_ready = true;
    ESP_LOGI(TAG, "wifi sta ready (saved=%d)", s_saved_valid);
}

mars_wifi_state_t mars_net_state(void)
{
    return s_state;
}

const char *mars_net_ssid(void)
{
    return s_connected_ssid;
}

esp_ip4_addr_t mars_net_ip(void)
{
    return s_ip;
}

int mars_net_rssi(void)
{
    if (s_state != MARS_WIFI_CONNECTED) return 0;
    wifi_ap_record_t info;
    if (esp_wifi_sta_get_ap_info(&info) == ESP_OK) return info.rssi;
    return 0;
}

bool mars_net_has_saved(void)
{
    return s_saved_valid;
}

esp_err_t mars_net_connect_checked(const char *ssid,const char *password,bool staged)
{
    if(!s_ready||!ssid||!ssid[0]||strlen(ssid)>32||(password&&strlen(password)>64))return ESP_ERR_INVALID_ARG;
    if(s_retry_timer)esp_timer_stop(s_retry_timer);
    s_retry_delay_ms=1000;s_pending_save=false;
    esp_wifi_disconnect();wifi_config_t cfg={0};memcpy(cfg.sta.ssid,ssid,strlen(ssid));
    if(password&&password[0]){memcpy(cfg.sta.password,password,strlen(password));cfg.sta.sae_pwe_h2e=WPA3_SAE_PWE_BOTH;}
    else cfg.sta.threshold.authmode=WIFI_AUTH_OPEN;
    esp_err_t err=esp_wifi_set_config(WIFI_IF_STA,&cfg);if(err!=ESP_OK)return err;
    strlcpy(s_connected_ssid,ssid,sizeof(s_connected_ssid));
    if(staged){s_credentials_saved=false;strlcpy(s_pending_ssid,ssid,sizeof(s_pending_ssid));strlcpy(s_pending_pass,password?password:"",sizeof(s_pending_pass));s_pending_save=true;}
    else{strlcpy(s_saved_ssid,ssid,sizeof(s_saved_ssid));strlcpy(s_saved_pass,password?password:"",sizeof(s_saved_pass));s_saved_valid=true;s_credentials_saved=save_credentials(s_saved_ssid,s_saved_pass);}
    set_state(MARS_WIFI_CONNECTING);err=esp_wifi_connect();
    if(err!=ESP_OK){s_pending_save=false;set_state(MARS_WIFI_FAILED);}
    return err;
}
void mars_net_connect(const char *ssid,const char *password)
{ esp_err_t err=mars_net_connect_checked(ssid,password,false);if(err!=ESP_OK)ESP_LOGW(TAG,"connect rejected: %s",esp_err_to_name(err)); }

void mars_net_disconnect(void)
{
    if (!s_ready) return;
    if (s_retry_timer) esp_timer_stop(s_retry_timer);
    esp_wifi_disconnect();
    set_state(MARS_WIFI_NONE);
}

void mars_net_forget(void)
{
    mars_net_disconnect();
    s_saved_valid = false;s_credentials_saved=false;s_pending_save=false;memset(s_pending_pass,0,sizeof(s_pending_pass));
    s_saved_ssid[0] = 0;
    s_saved_pass[0] = 0;
    nvs_handle_t handle;
    if (nvs_open("settings", NVS_READWRITE, &handle) == ESP_OK) {
        nvs_erase_key(handle, "wifi_ssid");
        nvs_erase_key(handle, "wifi_pass");
        nvs_commit(handle);
        nvs_close(handle);
    }
}

void mars_net_scan_start(void)
{
    if (!s_ready) return;
    s_ap_count = 0;
    ++s_scan_gen;
    wifi_scan_config_t scan = {
        .ssid = NULL,
        .bssid = NULL,
        .channel = 0,
        .show_hidden = true,
        .scan_type = WIFI_SCAN_TYPE_ACTIVE,
        .scan_time.active.min = 100,
        .scan_time.active.max = 300,
    };
    if (esp_wifi_scan_start(&scan, false) != ESP_OK) {
        /* 已在扫描时由系统接管，仅同步 generation 标记完成 */
        s_scan_done_gen = s_scan_gen;
    }
}

bool mars_net_scan_done(void)
{
    return s_scan_gen == s_scan_done_gen && s_scan_gen > 0;
}

int mars_net_scan_count(void)
{
    return s_ap_count;
}

const mars_wifi_ap_t *mars_net_scan_ap(int index)
{
    if (index < 0 || index >= s_ap_count) return NULL;
    return (const mars_wifi_ap_t *)&s_aps[index];
}

bool mars_net_wait_connected(uint32_t timeout_ms)
{
    const int64_t deadline = esp_timer_get_time() / 1000LL + (int64_t)timeout_ms;
    while (s_state != MARS_WIFI_CONNECTED) {
        if (esp_timer_get_time() / 1000LL >= deadline) return false;
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    return true;
}

void mars_net_set_power_save(bool enable)
{
    if (!s_ready) return;
    esp_wifi_set_ps(enable ? WIFI_PS_MIN_MODEM : WIFI_PS_NONE);
}

void mars_net_set_hostname(const char *hostname)
{
    esp_netif_t *sta = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (sta && hostname) esp_netif_set_hostname(sta, hostname);
}

esp_err_t mars_net_provision_start_checked(const char *ssid,const char *password)
{
    if(!s_ready||s_in_provision||!ssid||!ssid[0]||strlen(ssid)>31)return ESP_ERR_INVALID_STATE;
    if(password&&password[0]&&(strlen(password)<8||strlen(password)>63))return ESP_ERR_INVALID_ARG;
    if(s_retry_timer)esp_timer_stop(s_retry_timer);
    s_in_provision=true;s_pending_save=false;esp_wifi_disconnect();set_state(MARS_WIFI_NONE);
    if(!s_ap_if)s_ap_if=esp_netif_create_default_wifi_ap();
    if(!s_ap_if){s_in_provision=false;return ESP_ERR_NO_MEM;}
    esp_err_t err=esp_wifi_set_mode(WIFI_MODE_APSTA);
    if(err!=ESP_OK){s_in_provision=false;return err;}
    wifi_config_t ap={0};strlcpy((char *)ap.ap.ssid,ssid,sizeof(ap.ap.ssid));ap.ap.ssid_len=strlen(ssid);
    if(password&&password[0]){strlcpy((char *)ap.ap.password,password,sizeof(ap.ap.password));ap.ap.authmode=WIFI_AUTH_WPA2_PSK;}
    else ap.ap.authmode=WIFI_AUTH_OPEN;
    ap.ap.max_connection=2;
    err=esp_wifi_set_config(WIFI_IF_AP,&ap);
    if(err!=ESP_OK){s_in_provision=false;esp_wifi_set_mode(WIFI_MODE_STA);return err;}
    strlcpy(s_provision_ssid,ssid,sizeof(s_provision_ssid));mars_event_post(MARS_EVENT_WIFI_CONFIG_ENTER,s_provision_ssid,sizeof(s_provision_ssid));return ESP_OK;
}
void mars_net_provision_start(const char *ssid,const char *password)
{ esp_err_t err=mars_net_provision_start_checked(ssid,password);if(err!=ESP_OK)ESP_LOGW(TAG,"provision rejected: %s",esp_err_to_name(err)); }

void mars_net_provision_stop(void)
{
    if (!s_ready || !s_in_provision) return;
    bool connected=s_state==MARS_WIFI_CONNECTED;
    esp_wifi_set_mode(WIFI_MODE_STA);
    s_in_provision = false;s_pending_save=false;memset(s_pending_pass,0,sizeof(s_pending_pass));
    mars_event_post(MARS_EVENT_WIFI_CONFIG_EXIT, NULL, 0);
    if (s_saved_valid&&!connected) {
        strlcpy(s_connected_ssid, s_saved_ssid, sizeof(s_connected_ssid));
        esp_wifi_connect();
        set_state(MARS_WIFI_CONNECTING);
    }
}

bool mars_net_in_provision(void)
{
    return s_in_provision;
}

const char *mars_net_provision_ssid(void)
{
    return s_provision_ssid;
}

bool mars_net_credentials_saved(void){return s_credentials_saved;}
