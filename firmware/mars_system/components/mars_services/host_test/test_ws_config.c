#include "mars_ws.h"
#include "mars_ws_config.h"
#include "nvs.h"
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static pthread_t startup_thread;
static int scenario, accesses;

static void nvs_access(void)
{
    assert(pthread_equal(startup_thread, pthread_self()));
    ++accesses;
}

esp_err_t nvs_open(const char *name, int mode, nvs_handle_t *handle)
{
    nvs_access();
    assert(mode == NVS_READONLY);
    if (scenario == 1) return ESP_ERR_NVS_NOT_FOUND;
    *handle = strcmp(name, "websocket") == 0 ? 1 : 2;
    return ESP_OK;
}

esp_err_t nvs_get_i32(nvs_handle_t handle, const char *key, int32_t *value)
{
    nvs_access();
    assert(handle == 1 && !strcmp(key, "version"));
    *value = scenario == 2 ? 4 : 2;
    return ESP_OK;
}

esp_err_t nvs_get_str(nvs_handle_t handle, const char *key, char *out, size_t *size)
{
    nvs_access();
    const char *value;
    if (!strcmp(key, "url")) {
        assert(handle == 1);
        if (scenario == 6) { *size = 600; return 99; }
        value = scenario == 3 ? "ws://invalid.example" : "wss://valid.example/voice";
    } else if (!strcmp(key, "token")) {
        assert(handle == 1);
        if (scenario == 5) return ESP_ERR_NVS_NOT_FOUND;
        value = scenario == 4 ? "bad\r\nHeader: injected" : "dummy-test-token";
    } else {
        assert(handle == 2 && !strcmp(key, "uuid"));
        value = scenario == 7 ? "bad\nvalue" : "dummy-device-id";
    }
    assert(*size >= strlen(value) + 1);
    strcpy(out, value);
    *size = strlen(value) + 1;
    return ESP_OK;
}

void nvs_close(nvs_handle_t handle) { nvs_access(); assert(handle == 1 || handle == 2); }

static void *app_queries(void *argument)
{
    int expected = *(int *)argument;
    for (int i = 0; i < 1000; ++i) {
        uint32_t version = 99;
        assert(mars_ws_saved_config(&version) == expected);
        if (!expected) {
            assert(version == 2);
            const mars_ws_config_t *config = mars_ws_config_current();
            assert(!strcmp(config->url, "wss://valid.example/voice"));
            if (scenario == 5) assert(!config->token[0]);
            if (scenario == 7) assert(!config->uuid[0]);
        } else {
            assert(version == 99 && !mars_ws_config_current());
        }
    }
    return NULL;
}

int main(int argc, char **argv)
{
    assert(argc == 2);
    startup_thread = pthread_self();
    scenario = atoi(argv[1]);
    uint32_t version;
    assert(mars_ws_saved_config(&version) == -19);
    mars_ws_config_init();
    int expected = scenario == 1 || scenario == 3 || scenario == 6 ? -2
        : scenario == 2 || scenario == 4 ? -22 : 0;
    int after_startup = accesses;
    mars_ws_config_init();
    pthread_t threads[3];
    for (int i = 0; i < 3; ++i) assert(!pthread_create(&threads[i], NULL, app_queries, &expected));
    for (int i = 0; i < 3; ++i) pthread_join(threads[i], NULL);
    assert(accesses == after_startup);
    puts("PASS: startup-only NVS, immutable app reads, validation and header-injection protection");
}
