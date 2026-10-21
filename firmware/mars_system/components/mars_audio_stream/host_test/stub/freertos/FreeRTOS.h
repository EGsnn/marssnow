/* host 桩：FreeRTOS 核心类型与函数声明（pthread 实现，见 FreeRTOS_stub.c） */
#ifndef STUB_FREERTOS_H
#define STUB_FREERTOS_H

#include <stdint.h>
#include <stddef.h>

typedef uint32_t TickType_t;
typedef void *TaskHandle_t;
typedef void *SemaphoreHandle_t;
typedef void (*TaskFunction_t)(void *);

#define pdTRUE  1
#define pdFALSE 0
#define pdPASS  1
#define pdMS_TO_TICKS(ms) (TickType_t)(ms)
#define portMAX_DELAY (0xFFFFFFFFUL)

/* 桩实现的接口（测试可调用） */
void vTaskDelay(TickType_t ticks);
TickType_t xTaskGetTickCount(void);

#endif
