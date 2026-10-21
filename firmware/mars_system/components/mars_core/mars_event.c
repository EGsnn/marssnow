#include "mars_event.h"

#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#define TAG "mars_event"

#define MAX_SUBSCRIBERS 64

typedef struct {
    mars_event_id_t id;
    mars_event_callback_t cb;
    void *ctx;
    bool used;
} subscriber_t;

static subscriber_t s_subs[MAX_SUBSCRIBERS];
static SemaphoreHandle_t s_mutex;

void mars_event_init(void)
{
    if (s_mutex == NULL) {
        s_mutex = xSemaphoreCreateMutex();
    }
    memset(s_subs, 0, sizeof(s_subs));
}

static subscriber_t *find_slot(mars_event_id_t id, mars_event_callback_t cb, void *ctx)
{
    for (size_t i = 0; i < MAX_SUBSCRIBERS; ++i) {
        if (s_subs[i].used && s_subs[i].id == id && s_subs[i].cb == cb && s_subs[i].ctx == ctx) {
            return &s_subs[i];
        }
    }
    return NULL;
}

bool mars_event_subscribe(mars_event_id_t id, mars_event_callback_t cb, void *ctx)
{
    if (cb == NULL || id == MARS_EVENT_NONE) {
        return false;
    }
    if (s_mutex == NULL) {
        mars_event_init();
    }
    if (find_slot(id, cb, ctx) != NULL) {
        return true; /* 已注册 */
    }
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    for (size_t i = 0; i < MAX_SUBSCRIBERS; ++i) {
        if (!s_subs[i].used) {
            s_subs[i].id = id;
            s_subs[i].cb = cb;
            s_subs[i].ctx = ctx;
            s_subs[i].used = true;
            xSemaphoreGive(s_mutex);
            return true;
        }
    }
    xSemaphoreGive(s_mutex);
    ESP_LOGW(TAG, "subscriber table full");
    return false;
}

bool mars_event_unsubscribe(mars_event_id_t id, mars_event_callback_t cb, void *ctx)
{
    if (s_mutex == NULL) {
        return false;
    }
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    subscriber_t *slot = find_slot(id, cb, ctx);
    if (slot != NULL) {
        slot->used = false;
        xSemaphoreGive(s_mutex);
        return true;
    }
    xSemaphoreGive(s_mutex);
    return false;
}

void mars_event_post(mars_event_id_t id, const void *data, size_t data_size)
{
    const mars_event_t event = {.id = id, .data = data, .data_size = data_size};
    for (size_t i = 0; i < MAX_SUBSCRIBERS; ++i) {
        if (s_subs[i].used && s_subs[i].id == id && s_subs[i].cb != NULL) {
            s_subs[i].cb(&event, s_subs[i].ctx);
        }
    }
}
