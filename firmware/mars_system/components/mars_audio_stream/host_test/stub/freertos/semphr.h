/* host 桩：semphr 声明（实现见 FreeRTOS_stub.c） */
#ifndef STUB_SEMPHR_H
#define STUB_SEMPHR_H

#include "freertos/FreeRTOS.h"

SemaphoreHandle_t xSemaphoreCreateMutex(void);
SemaphoreHandle_t xSemaphoreCreateCounting(int max_count, int init_count);
SemaphoreHandle_t xSemaphoreCreateBinary(void);
int xSemaphoreTake(SemaphoreHandle_t sem, TickType_t timeout_ticks);
int xSemaphoreGive(SemaphoreHandle_t sem);
void vSemaphoreDelete(SemaphoreHandle_t sem);

/* 故障注入：使接下来 n 次 mutex/counting 创建返回 NULL（0=关闭） */
void stub_sem_fail_next(int n);

#endif
