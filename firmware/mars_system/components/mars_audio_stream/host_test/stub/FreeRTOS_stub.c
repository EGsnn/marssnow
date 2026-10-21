/* host 桩：FreeRTOS pthread 实现
 *  - 计数信号量/互斥量用 pthread mutex + cond 实现
 *  - 任务 = detached pthread；vTaskDelete(NULL) = pthread_exit
 *  - 系统 tick 全局递增（并发安全），vTaskDelay 换算 usleep + tick 累加
 */
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

enum sem_kind { SEM_MUTEX, SEM_COUNTING };

typedef struct {
    enum sem_kind kind;
    pthread_mutex_t mu;
    pthread_cond_t cv;
    int count;
    int max;
} stub_sem_t;

/* ── tick ── */
static pthread_mutex_t g_tick_mu = PTHREAD_MUTEX_INITIALIZER;
static uint32_t g_tick = 0;

TickType_t xTaskGetTickCount(void)
{
    uint32_t t;
    pthread_mutex_lock(&g_tick_mu);
    t = g_tick;
    pthread_mutex_unlock(&g_tick_mu);
    return t;
}

void vTaskDelay(TickType_t ticks)
{
    if (ticks > 0) {
        usleep((useconds_t)ticks * 1000);
    }
    pthread_mutex_lock(&g_tick_mu);
    g_tick += ticks;
    pthread_mutex_unlock(&g_tick_mu);
}

/* ── 信号量 ── */

/* 故障注入：从现在起第 n 次 semaphore/mutex 创建返回 NULL（0=关闭） */
static int g_sem_fail_next = 0;
/* 故障注入：从现在起第 n 次任务创建失败（返回 0）（0=关闭） */
static int g_task_fail_next = 0;
/* 任务创建计数（按任务名）：用于断言"最多一个编码/解码任务" */
static int g_enc_create_count = 0;
static int g_dec_create_count = 0;
static pthread_mutex_t g_ctl_mu = PTHREAD_MUTEX_INITIALIZER;

void stub_sem_fail_next(int n)
{
    pthread_mutex_lock(&g_ctl_mu);
    g_sem_fail_next = n;
    pthread_mutex_unlock(&g_ctl_mu);
}
void stub_task_fail_next(int n)
{
    pthread_mutex_lock(&g_ctl_mu);
    g_task_fail_next = n;
    pthread_mutex_unlock(&g_ctl_mu);
}
int stub_enc_task_create_count(void)
{
    pthread_mutex_lock(&g_ctl_mu);
    int n = g_enc_create_count;
    pthread_mutex_unlock(&g_ctl_mu);
    return n;
}
int stub_dec_task_create_count(void)
{
    pthread_mutex_lock(&g_ctl_mu);
    int n = g_dec_create_count;
    pthread_mutex_unlock(&g_ctl_mu);
    return n;
}
void stub_task_counters_reset(void)
{
    pthread_mutex_lock(&g_ctl_mu);
    g_enc_create_count = g_dec_create_count = 0;
    pthread_mutex_unlock(&g_ctl_mu);
}

static int consume_fail_counter(int *counter)
{
    pthread_mutex_lock(&g_ctl_mu);
    int fail = 0;
    if (*counter > 0) {
        (*counter)--;
        fail = (*counter == 0);
    }
    pthread_mutex_unlock(&g_ctl_mu);
    return fail;
}

static int64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

SemaphoreHandle_t xSemaphoreCreateMutex(void)
{
    if (consume_fail_counter(&g_sem_fail_next)) {
        return NULL;
    }
    stub_sem_t *s = calloc(1, sizeof(*s));
    if (s == NULL) {
        return NULL;
    }
    s->kind = SEM_MUTEX;
    pthread_mutex_init(&s->mu, NULL);
    return s;
}

SemaphoreHandle_t xSemaphoreCreateCounting(int max_count, int init_count)
{
    if (consume_fail_counter(&g_sem_fail_next)) {
        return NULL;
    }
    stub_sem_t *s = calloc(1, sizeof(*s));
    if (s == NULL) {
        return NULL;
    }
    s->kind = SEM_COUNTING;
    s->max = max_count;
    s->count = init_count;
    pthread_mutex_init(&s->mu, NULL);
    pthread_cond_init(&s->cv, NULL);
    return s;
}

/* 二进制信号量 = max 1、初始 0 的计数信号量（任务退出屏障用） */
SemaphoreHandle_t xSemaphoreCreateBinary(void)
{
    return xSemaphoreCreateCounting(1, 0);
}

static int wait_until(clockid_t clock, int64_t deadline_ms, pthread_mutex_t *mu,
                      pthread_cond_t *cv, int (*pred)(void *), void *arg)
{
    while (!pred(arg)) {
        struct timespec ts;
        clock_gettime(clock, &ts);
        ts.tv_sec = (time_t)(deadline_ms / 1000);
        ts.tv_nsec = (long)(deadline_ms % 1000) * 1000000L;
        int rc = pthread_cond_timedwait(cv, mu, &ts);
        if (rc == ETIMEDOUT) {
            return 0;
        }
    }
    return 1;
}

static int count_pred(void *arg)
{
    stub_sem_t *s = arg;
    return s->count > 0;
}

int xSemaphoreTake(SemaphoreHandle_t sem, TickType_t timeout_ticks)
{
    stub_sem_t *s = sem;
    if (s == NULL) {
        return pdFALSE;
    }
    if (s->kind == SEM_MUTEX) {
        /* mutex：直接操作 pthread mutex（不经过内部保护锁，避免自锁） */
        if (timeout_ticks == 0 || timeout_ticks == portMAX_DELAY) {
            pthread_mutex_lock(&s->mu);
            return pdTRUE;
        }
        for (int i = 0; i < (int)timeout_ticks; i += 5) {
            if (pthread_mutex_trylock(&s->mu) == 0) {
                return pdTRUE;
            }
            usleep(5000);
        }
        return pdFALSE;
    }
    /* counting sem */
    pthread_mutex_lock(&s->mu);
    if (s->count == 0) {
        if (timeout_ticks == 0) {
            pthread_mutex_unlock(&s->mu);
            return pdFALSE;
        }
        int64_t deadline = now_ms() + (timeout_ticks == portMAX_DELAY ? 60000 : timeout_ticks);
        if (!wait_until(CLOCK_REALTIME, deadline, &s->mu, &s->cv, count_pred, s)) {
            pthread_mutex_unlock(&s->mu);
            return pdFALSE;
        }
    }
    s->count--;
    pthread_mutex_unlock(&s->mu);
    return pdTRUE;
}

int xSemaphoreGive(SemaphoreHandle_t sem)
{
    stub_sem_t *s = sem;
    if (s == NULL) {
        return pdFALSE;
    }
    if (s->kind == SEM_MUTEX) {
        pthread_mutex_unlock(&s->mu);
        return pdTRUE;
    }
    pthread_mutex_lock(&s->mu);
    if (s->count < s->max) {
        s->count++;
        pthread_cond_signal(&s->cv);
        pthread_mutex_unlock(&s->mu);
        return pdTRUE;
    }
    pthread_mutex_unlock(&s->mu);
    return pdFALSE;
}

void vSemaphoreDelete(SemaphoreHandle_t sem)
{
    stub_sem_t *s = sem;
    if (s == NULL) {
        return;
    }
    pthread_mutex_destroy(&s->mu);
    if (s->kind == SEM_COUNTING) {
        pthread_cond_destroy(&s->cv);
    }
    free(s);
}

/* ── 任务 ── */
static pthread_mutex_t g_task_mu = PTHREAD_MUTEX_INITIALIZER;
static int g_tasks_running = 0;
/* 按方向统计"同时存活任务"峰值：每次测试结束断言 enc/dec 峰值各 ≤1。 */
static int g_enc_running = 0, g_dec_running = 0;
static int g_enc_peak = 0, g_dec_peak = 0;

/* 退出任务的名字（vTaskDelete 不再传 handle，用线程局部记录） */
static __thread const char *g_current_task_name = NULL;

typedef struct {
    TaskFunction_t fn;
    const char *name;
} task_arg_t;

static void *task_wrapper(void *arg)
{
    task_arg_t *ta = arg;
    TaskFunction_t fn = ta->fn;
    g_current_task_name = ta->name;
    free(ta);
    fn(NULL);
    return NULL;
}

/* 记录一个任务开始/结束（按名字归类），维护峰值。 */
static void task_live_change(const char *name, int delta)
{
    pthread_mutex_lock(&g_task_mu);
    if (name != NULL) {
        if (strcmp(name, "mars_enc") == 0) {
            g_enc_running += delta;
            if (delta > 0 && g_enc_running > g_enc_peak) g_enc_peak = g_enc_running;
        } else if (strcmp(name, "mars_dec") == 0) {
            g_dec_running += delta;
            if (delta > 0 && g_dec_running > g_dec_peak) g_dec_peak = g_dec_running;
        }
    }
    pthread_mutex_unlock(&g_task_mu);
}

int xTaskCreate(TaskFunction_t fn, const char *name, uint32_t stack,
                void *arg, int prio, TaskHandle_t *handle)
{
    (void)stack;
    (void)prio;
    (void)arg;
    if (consume_fail_counter(&g_task_fail_next)) {
        return 0;   /* != pdPASS */
    }
    task_arg_t *ta = malloc(sizeof(*ta));
    if (ta == NULL) {
        return 0;
    }
    ta->fn = fn;
    ta->name = name;
    /* 先登记存活状态再创建线程，避免极短任务先退出导致计数变成负数。 */
    pthread_mutex_lock(&g_task_mu);
    g_tasks_running++;
    pthread_mutex_unlock(&g_task_mu);
    task_live_change(name, +1);
    pthread_t tid;
    if (pthread_create(&tid, NULL, task_wrapper, ta) != 0) {
        task_live_change(name, -1);
        pthread_mutex_lock(&g_task_mu);
        g_tasks_running--;
        pthread_mutex_unlock(&g_task_mu);
        free(ta);
        return 0;   /* != pdPASS */
    }
    pthread_detach(tid);
    pthread_mutex_lock(&g_ctl_mu);
    if (name != NULL) {
        if (strcmp(name, "mars_enc") == 0) g_enc_create_count++;
        else if (strcmp(name, "mars_dec") == 0) g_dec_create_count++;
    }
    pthread_mutex_unlock(&g_ctl_mu);
    if (handle != NULL) {
        *handle = (void *)(uintptr_t)tid;
    }
    return pdPASS;
}

void vTaskDelete(TaskHandle_t task)
{
    if (task != NULL) {
        pthread_cancel((pthread_t)(uintptr_t)task);
    }
    task_live_change(g_current_task_name, -1);
    pthread_mutex_lock(&g_task_mu);
    g_tasks_running--;
    pthread_mutex_unlock(&g_task_mu);
    pthread_exit(NULL);
}

/* 测试辅助：每方向同时存活任务峰值（应恒 ≤1） */
int stub_enc_peak(void)
{
    pthread_mutex_lock(&g_task_mu);
    int n = g_enc_peak;
    pthread_mutex_unlock(&g_task_mu);
    return n;
}
int stub_dec_peak(void)
{
    pthread_mutex_lock(&g_task_mu);
    int n = g_dec_peak;
    pthread_mutex_unlock(&g_task_mu);
    return n;
}
void stub_peak_reset(void)
{
    pthread_mutex_lock(&g_task_mu);
    g_enc_peak = g_dec_peak = 0;
    pthread_mutex_unlock(&g_task_mu);
}

/* 测试辅助 */
int stub_tasks_running(void)
{
    int n;
    pthread_mutex_lock(&g_task_mu);
    n = g_tasks_running;
    pthread_mutex_unlock(&g_task_mu);
    return n;
}
