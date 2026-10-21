/* host 桩：task 声明（实现见 FreeRTOS_stub.c） */
#ifndef STUB_TASK_H
#define STUB_TASK_H

#include "freertos/FreeRTOS.h"

int xTaskCreate(TaskFunction_t fn, const char *name, uint32_t stack,
                void *arg, int prio, TaskHandle_t *handle);
void vTaskDelete(TaskHandle_t task);

/* 故障注入与计数（test_service.c 使用） */
void stub_task_fail_next(int n);
int stub_enc_task_create_count(void);
int stub_dec_task_create_count(void);
void stub_task_counters_reset(void);
int stub_tasks_running(void);
/* 同时存活任务峰值（每方向应恒 ≤1；测试结束断言） */
int stub_enc_peak(void);
int stub_dec_peak(void);
void stub_peak_reset(void);

#endif
