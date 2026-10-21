/* host 桩：esp_log（输出到 stdout，含 P/F 前缀便于机器判定） */
#ifndef STUB_ESP_LOG_H
#define STUB_ESP_LOG_H

#include <stdio.h>

#define ESP_LOGI(tag, ...)  do { printf("[I][%s] ", tag); printf(__VA_ARGS__); printf("\n"); } while (0)
#define ESP_LOGW(tag, ...)  do { printf("[W][%s] ", tag); printf(__VA_ARGS__); printf("\n"); } while (0)
#define ESP_LOGE(tag, ...)  do { printf("[E][%s] ", tag); printf(__VA_ARGS__); printf("\n"); } while (0)

typedef int esp_err_t;
#define ESP_OK 0
#define ESP_FAIL (-1)
#define ESP_ERR_INVALID_STATE 0x103   /* 259，与 ESP-IDF 一致 */
#define ESP_ERR_TIMEOUT       0x107   /* 263 */
#define pdPASS 1

static inline const char *esp_err_to_name(int err) { (void)err; return "ESP_ERR"; }

#endif
