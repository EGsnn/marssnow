#include "mars_ws.h"
#include "mars_ws_reassembly.h"
#include "mars_ws_config.h"
#include <stdbool.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_websocket_client.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#define QUEUE_DEPTH 16
typedef struct { mars_ws_event_t event; uint8_t data[MARS_WS_MAX_MESSAGE]; } ws_item_t;
static SemaphoreHandle_t s_api_lock, s_queue_lock;
static esp_websocket_client_handle_t s_client;
static ws_item_t *s_items;
static uint8_t *s_message;
static unsigned s_head, s_count;
static mars_ws_reassembly_t s_reassembly;
static atomic_int s_fault;
static atomic_bool s_connected;
static atomic_bool s_closing;
static void memory_snapshot(const char *stage)
{
    ESP_LOGI("mars_ws", "MEM[%s] internal=%u psram=%u", stage,
        (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
        (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
}

void mars_ws_init(void)
{
    if (!s_api_lock) s_api_lock = xSemaphoreCreateMutex();
    if (!s_queue_lock) s_queue_lock = xSemaphoreCreateMutex();
    mars_ws_config_init();
}

int mars_ws_saved_config(uint32_t *version)
{
    const mars_ws_config_t *config = mars_ws_config_current();
    if (!config) return mars_ws_config_status();
    if (version) *version = config->version;
    return 0;
}

static void enqueue(uint32_t type, const void *data, size_t len)
{
    if (!s_items || len > MARS_WS_MAX_MESSAGE) { atomic_store(&s_fault, -90); return; }
    TickType_t started = xTaskGetTickCount();
    while (!atomic_load(&s_closing)) {
        xSemaphoreTake(s_queue_lock, portMAX_DELAY);
        if (s_count < QUEUE_DEPTH) {
            ws_item_t *item = &s_items[(s_head + s_count) % QUEUE_DEPTH];
            item->event = (mars_ws_event_t){ .type = type, .length = len };
            if (len) memcpy(item->data, data, len);
            ++s_count;
            xSemaphoreGive(s_queue_lock);
            return;
        }
        xSemaphoreGive(s_queue_lock);
        /* Apply TCP backpressure while playback catches up. Close sets
         * s_closing before joining the worker, so this wait cannot deadlock. */
        if (xTaskGetTickCount() - started > pdMS_TO_TICKS(1000)) {
            atomic_store(&s_fault, -105); return;
        }
        vTaskDelay(1);
    }
}

static void on_ws_event(void *arg, esp_event_base_t base, int32_t id, void *payload)
{
    (void)arg; (void)base;
    if (id == WEBSOCKET_EVENT_CONNECTED) {
        memory_snapshot("connected");
        atomic_store(&s_connected, true);
        enqueue(MARS_WS_CONNECTED, NULL, 0);
    } else if (id == WEBSOCKET_EVENT_DISCONNECTED || id == WEBSOCKET_EVENT_CLOSED) {
        atomic_store(&s_connected, false);
        s_reassembly = (mars_ws_reassembly_t){ .data = s_message, .capacity = MARS_WS_MAX_MESSAGE };
        enqueue(MARS_WS_DISCONNECTED, NULL, 0);
    } else if (id == WEBSOCKET_EVENT_ERROR) {
        atomic_store(&s_connected, false);
        atomic_store(&s_fault, -5);
    } else if (id == WEBSOCKET_EVENT_DATA) {
        esp_websocket_event_data_t *e = payload;
        if (e->op_code >= 8) return; /* library handles ping/pong/close */
        if (e->data_len < 0 || e->payload_len < 0 || e->payload_offset < 0 ||
            e->payload_offset > e->payload_len ||
            e->data_len > e->payload_len - e->payload_offset) {
            atomic_store(&s_fault, -71); return;
        }
        int rc = mars_ws_reassembly_feed(&s_reassembly, e->op_code, e->fin,
            e->payload_len, e->payload_offset, e->data_ptr, e->data_len);
        if (rc < 0) atomic_store(&s_fault, rc);
        else if (rc == 1) {
            enqueue(s_reassembly.opcode == 1 ? MARS_WS_TEXT : MARS_WS_BINARY,
                    s_message, s_reassembly.length);
            s_reassembly.opcode = 0; s_reassembly.length = 0;
        }
    }
}

static void destroy_locked(void)
{
    atomic_store(&s_closing, true);
    if (s_client) {
        /* stop/destroy join the component worker; no callback survives return. */
        esp_websocket_client_stop(s_client);
        esp_websocket_client_destroy(s_client);
        s_client = NULL;
    }
    free(s_items); free(s_message);
    s_items = NULL; s_message = NULL;
    s_head = s_count = 0; memset(&s_reassembly, 0, sizeof(s_reassembly));
    atomic_store(&s_connected, false);
    atomic_store(&s_fault, 0);
    memory_snapshot("closed");
}

int mars_ws_open_saved(void)
{
    if (!s_api_lock || !s_queue_lock) return -19;
    xSemaphoreTake(s_api_lock, portMAX_DELAY);
    if (s_client) { xSemaphoreGive(s_api_lock); return -16; }
    const mars_ws_config_t *config = mars_ws_config_current();
    if (!config) { xSemaphoreGive(s_api_lock); return mars_ws_config_status(); }
    char uuid[64], headers[1280], mac[18];
    snprintf(uuid, sizeof(uuid), "%s", config->uuid);
    uint32_t version = config->version;
    uint8_t address[6]; esp_read_mac(address, ESP_MAC_WIFI_STA);
    snprintf(mac, sizeof(mac), "%02x:%02x:%02x:%02x:%02x:%02x", address[0], address[1],
             address[2], address[3], address[4], address[5]);
    if (!uuid[0]) snprintf(uuid, sizeof(uuid), "mars-%02x%02x%02x%02x%02x%02x",
                          address[0],address[1],address[2],address[3],address[4],address[5]);
    int n = snprintf(headers, sizeof(headers),
        "Protocol-Version: %u\r\nDevice-Id: %s\r\nClient-Id: %s\r\n", (unsigned)version, mac, uuid);
    if (config->token[0]) snprintf(headers + n, sizeof(headers) - n, "Authorization: %s%s\r\n",
                          strchr(config->token, ' ') ? "" : "Bearer ", config->token);
    s_items = heap_caps_calloc(QUEUE_DEPTH, sizeof(ws_item_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_message = heap_caps_malloc(MARS_WS_MAX_MESSAGE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_items || !s_message) { destroy_locked(); xSemaphoreGive(s_api_lock); return -12; }
    s_reassembly = (mars_ws_reassembly_t){ .data = s_message, .capacity = MARS_WS_MAX_MESSAGE };
    atomic_store(&s_closing, false);
    /* Avoid third-party DEBUG logs containing a URI or HTTP credential headers. */
    esp_log_level_set("websocket_client", ESP_LOG_WARN);
    esp_log_level_set("transport_ws", ESP_LOG_WARN);
    esp_websocket_client_config_t cfg = {
        .uri = config->url, .headers = headers, .crt_bundle_attach = esp_crt_bundle_attach,
        .disable_auto_reconnect = true, .network_timeout_ms = 5000,
        .task_stack = 8192, .buffer_size = 2048, .ping_interval_sec = 20,
        .pingpong_timeout_sec = 40,
    };
    s_client = esp_websocket_client_init(&cfg);
    memset(headers, 0, sizeof(headers));
    if (!s_client || esp_websocket_register_events(s_client, WEBSOCKET_EVENT_ANY, on_ws_event, NULL) != ESP_OK ||
        esp_websocket_client_start(s_client) != ESP_OK) {
        destroy_locked(); xSemaphoreGive(s_api_lock); return -5;
    }
    ESP_LOGI("mars_ws", "WSS connection started (protocol=%u, verified TLS)", (unsigned)version);
    xSemaphoreGive(s_api_lock);
    return 0;
}

static int send_message(const void *data, size_t len, bool binary)
{
    if (!data || !len || len > MARS_WS_MAX_MESSAGE || !s_api_lock) return -22;
    xSemaphoreTake(s_api_lock, portMAX_DELAY);
    int rc = -107;
    if (s_client && atomic_load(&s_connected)) {
        int n = binary ? esp_websocket_client_send_bin(s_client, data, len, pdMS_TO_TICKS(1000))
                       : esp_websocket_client_send_text(s_client, data, len, pdMS_TO_TICKS(1000));
        rc = n == (int)len ? 0 : -5;
    }
    xSemaphoreGive(s_api_lock);
    return rc;
}
int mars_ws_send_text(const char *data, size_t len) { return send_message(data, len, false); }
int mars_ws_send_binary(const void *data, size_t len) { return send_message(data, len, true); }

int mars_ws_receive(void *buffer, size_t capacity, mars_ws_event_t *event, uint32_t timeout_ms)
{
    if (!event || !s_api_lock) return -22;
    TickType_t end = xTaskGetTickCount() + pdMS_TO_TICKS(timeout_ms);
    do {
        xSemaphoreTake(s_api_lock, portMAX_DELAY);
        int fault = atomic_exchange(&s_fault, 0);
        if (fault) {
            *event = (mars_ws_event_t){ .type = MARS_WS_ERROR, .error = fault };
            xSemaphoreGive(s_api_lock); return 0;
        }
        xSemaphoreTake(s_queue_lock, portMAX_DELAY);
        if (s_count) {
            ws_item_t *item = &s_items[s_head];
            if (item->event.length > capacity || (item->event.length && !buffer)) {
                xSemaphoreGive(s_queue_lock); xSemaphoreGive(s_api_lock); return -90;
            }
            *event = item->event;
            if (event->length) memcpy(buffer, item->data, event->length);
            s_head = (s_head + 1) % QUEUE_DEPTH; --s_count;
            xSemaphoreGive(s_queue_lock); xSemaphoreGive(s_api_lock); return 0;
        }
        xSemaphoreGive(s_queue_lock); xSemaphoreGive(s_api_lock);
        if (!timeout_ms || (int32_t)(xTaskGetTickCount() - end) >= 0) break;
        vTaskDelay(pdMS_TO_TICKS(5));
    } while (true);
    return -11;
}

int mars_ws_close(void)
{
    if (!s_api_lock) return 0;
    xSemaphoreTake(s_api_lock, portMAX_DELAY);
    destroy_locked();
    xSemaphoreGive(s_api_lock);
    return 0;
}
