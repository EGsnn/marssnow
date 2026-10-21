#include "mars_app.h"

#include <string.h>

#include "esp_log.h"
#include "mars_event.h"

#define TAG "mars_app"

#define MAX_APPS 16

static const mars_app_t *s_apps[MAX_APPS];
static size_t s_app_count;

/* 当前打开的应用名（供动态应用桥定位槽位） */
const char *g_mars_active_app_name = NULL;

bool mars_app_register(const mars_app_t *app)
{
    if (app == NULL || app->name == NULL || app->open == NULL) {
        return false;
    }
    if (s_app_count >= MAX_APPS) {
        ESP_LOGW(TAG, "app table full");
        return false;
    }
    s_apps[s_app_count++] = app;
    return true;
}

size_t mars_app_count(void)
{
    return s_app_count;
}

const mars_app_t *mars_app_get(size_t index)
{
    if (index >= s_app_count) {
        return NULL;
    }
    return s_apps[index];
}

bool mars_app_open(size_t index)
{
    const mars_app_t *app = mars_app_get(index);
    if (app == NULL) {
        return false;
    }
    g_mars_active_app_name = app->name;
    if (app->open != NULL) {
        app->open();
    }
    static int event_index;
    event_index = (int)index;
    mars_event_post(MARS_EVENT_APP_OPEN, &event_index, sizeof(event_index));
    return true;
}

bool mars_app_close(size_t index)
{
    const mars_app_t *app = mars_app_get(index);
    if (app == NULL) {
        return false;
    }
    if (app->close != NULL) {
        app->close();
    }
    static int event_index;
    event_index = (int)index;
    mars_event_post(MARS_EVENT_APP_CLOSE, &event_index, sizeof(event_index));
    return true;
}
