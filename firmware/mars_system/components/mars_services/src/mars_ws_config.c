#include "mars_ws_config.h"
#include <stdbool.h>
#include <stddef.h>
#include <string.h>
#include "nvs.h"

static mars_ws_config_t saved;
static int status = -19;
static bool initialized;

static int read_string(nvs_handle_t handle, const char *key, char *out, size_t capacity)
{
    size_t length = capacity;
    out[0] = 0;
    esp_err_t error = nvs_get_str(handle, key, out, &length);
    if (error != ESP_OK) return error == ESP_ERR_NVS_NOT_FOUND ? -2 : -22;
    if (!length || length > capacity || out[length - 1] != 0 || strchr(out, '\r') || strchr(out, '\n')) {
        out[0] = 0;
        return -22;
    }
    return 0;
}

void mars_ws_config_init(void)
{
    if (initialized) return;
    initialized = true;
    nvs_handle_t handle;
    if (nvs_open("websocket", NVS_READONLY, &handle) != ESP_OK) {
        status = -2;
        return;
    }
    int32_t version = 1;
    int url = read_string(handle, "url", saved.url, sizeof(saved.url));
    int token = read_string(handle, "token", saved.token, sizeof(saved.token));
    nvs_get_i32(handle, "version", &version);
    nvs_close(handle);
    if (url || strncmp(saved.url, "wss://", 6) != 0) status = -2;
    else if (version < 1 || version > 3 || token == -22) status = -22;
    else status = 0;
    if (status) {
        memset(&saved, 0, sizeof(saved));
        return;
    }
    saved.version = (uint32_t)version;
    if (nvs_open("board", NVS_READONLY, &handle) == ESP_OK) {
        if (read_string(handle, "uuid", saved.uuid, sizeof(saved.uuid))) saved.uuid[0] = 0;
        nvs_close(handle);
    }
}

int mars_ws_config_status(void) { return status; }
const mars_ws_config_t *mars_ws_config_current(void) { return status == 0 ? &saved : NULL; }
