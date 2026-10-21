#include "mars_app_preferences.h"
#include "nvs.h"
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#ifndef MARS_HOST_TEST
#include "esp_heap_caps.h"
#include "freertos/idf_additions.h"
#include "esp_log.h"
#endif

static int valid_name(const char *s)
{
    if (!s) return 0;
    size_t n = 0;
    for (; s[n]; ++n) {
        if (n >= 15 || !((s[n] >= 'a' && s[n] <= 'z') ||
            (s[n] >= 'A' && s[n] <= 'Z') || (s[n] >= '0' && s[n] <= '9') || s[n] == '_')) return 0;
    }
    return n > 0;
}
static int valid(const char *ns, const char *key)
{ return valid_name(ns) && strlen(ns) > 4 && !strncmp(ns,"app_",4) && valid_name(key); }
static int read_pref(const char *ns, const char *key, uint32_t *value)
{
    if (!value || !valid(ns,key)) return -22;
    nvs_handle_t h;
    esp_err_t rc = nvs_open(ns,NVS_READONLY,&h);
    if (rc != ESP_OK) return rc == ESP_ERR_NVS_NOT_FOUND ? -2 : -5;
    uint32_t v;
    rc = nvs_get_u32(h,key,&v); nvs_close(h);
    if (rc == ESP_OK) *value = v;
    return rc == ESP_OK ? 0 : rc == ESP_ERR_NVS_NOT_FOUND ? -2 : -5;
}
static int write_pref(const char *ns, const char *key, uint32_t value)
{
    if (!valid(ns,key)) return -22;
    nvs_handle_t h;
    if (nvs_open(ns,NVS_READWRITE,&h) != ESP_OK) return -5;
    esp_err_t rc = nvs_set_u32(h,key,value);
    if (rc == ESP_OK) rc = nvs_commit(h);
    nvs_close(h); return rc == ESP_OK ? 0 : -5;
}

/* Dynamic app stacks are in PSRAM. NVS may disable the flash/PSRAM cache:
 * never execute NVS on that stack, nor hand it pointers into app memory.
 * A single internal-stack worker handles copied requests synchronously. */
static SemaphoreHandle_t s_lock, s_work, s_done;
static TaskHandle_t s_worker;
static struct {
    char ns[16], key[16];
    uint32_t value;
    int write, result;
} s_request;
static void pref_worker(void *arg)
{
    (void)arg;
    for (;;) {
        xSemaphoreTake(s_work,portMAX_DELAY);
        if(s_request.write) s_request.result=write_pref(s_request.ns,s_request.key,s_request.value);
        else s_request.result=read_pref(s_request.ns,s_request.key,&s_request.value);
        xSemaphoreGive(s_done);
    }
}
int mars_app_preferences_init(void)
{
    /* Startup-only, before any dynamic app can call these services. */
    if(s_worker)return 0;
    s_lock=xSemaphoreCreateMutex();s_work=xSemaphoreCreateBinary();s_done=xSemaphoreCreateBinary();
    if(s_lock && s_work && s_done) {
#ifdef MARS_HOST_TEST
        int rc=xTaskCreate(pref_worker,"app_nvs",4096,NULL,4,&s_worker);
#else
        int rc=xTaskCreateWithCaps(pref_worker,"app_nvs",4096,NULL,4,&s_worker,
                                  MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
#endif
        if(rc==pdPASS) {
#ifndef MARS_HOST_TEST
            ESP_LOGI("app_preferences","worker ready (internal RAM stack)");
#endif
            return 0;
        }
    }
    if(s_lock)vSemaphoreDelete(s_lock);
    if(s_work)vSemaphoreDelete(s_work);
    if(s_done)vSemaphoreDelete(s_done);
    s_lock=s_work=s_done=NULL;s_worker=NULL;return -5;
}
static int request(const char *ns,const char *key,uint32_t *value,int write)
{
    if(!value || !valid(ns,key))return -22;
    if(!s_worker)return -5;
    xSemaphoreTake(s_lock,portMAX_DELAY);
    strcpy(s_request.ns,ns);strcpy(s_request.key,key);
    s_request.write=write;s_request.value=write?*value:0;
    xSemaphoreGive(s_work);
    xSemaphoreTake(s_done,portMAX_DELAY); /* no app unload before completion */
    int rc=s_request.result;
    if(!rc && !write)*value=s_request.value;
    xSemaphoreGive(s_lock);return rc;
}
int mars_app_pref_get_u32(const char *ns,const char *key,uint32_t *value)
{ return request(ns,key,value,0); }
int mars_app_pref_set_u32(const char *ns,const char *key,uint32_t value)
{ return request(ns,key,&value,1); }
