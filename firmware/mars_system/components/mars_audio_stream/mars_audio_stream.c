/* mars_audio_stream：火星系统 Opus 流式音频服务（P1）
 *
 * 架构（全部在系统侧，动态应用只通过 mars_audio_stream.h 的 ABI 访问）：
 *
 *   上行（采集 → 60ms Opus → 应用轮询）：
 *     mars_media_record_start(cb) --80ms/1280样本@16k--> 拼帧器(60ms/960)
 *         --编码任务(esp_opus_enc, 一次唤醒取满全部完整帧)--> 有界包队列
 *         --mars_audio_stream_read()--> 应用
 *
 *   下行（应用提交 Opus → 系统解码播放）：
 *     mars_audio_play_opus() --有界包队列--> 解码任务(esp_opus_dec)
 *         --PCM--> 24k/48k? --mars_resample--> 16k --mars_media_play()--> ES8311
 *
 * 线程安全（上下行各自独立、互不阻塞）：
 *   - 上行：record 回调(读任务) 与 编码任务 经 s_acc_mutex 保护拼帧器；
 *           编码任务 push 与 应用 read/pop 经 s_up_q_mutex 保护队列。
 *   - 下行：应用 push 与 解码任务 pop 经 s_down_q_mutex 保护队列。
 *   - 锁不嵌套：acc 锁内不取队列锁，避免死锁。
 *
 * 生命周期（同步屏障）：
 *   - stop()：停录音(等读任务退) → 唤醒编码任务 → 等任务自然退出(禁强删) →
 *     close 编码器 → free 输出缓冲 → 清空队列。返回后系统任务不再触碰
 *     应用内存（严禁异步回调已卸载应用代码：任务只访问本模块私有内存）。
 *   - playback_stop()：停播放任务 → close 解码器 → 清队列 → 关闭本服务
 *     开启的输出通路。
 *
 * 资源所有权：up/down 各自完整独立初始化，部分分配失败时回滚已建资源；
 * stop/start 重复调用幂等且不泄漏。
 */

#include "mars_audio_stream.h"

#include <stdlib.h>
#include <stdatomic.h>
#include <string.h>

#if defined(MARS_HOST_TEST)
#include "host_malloc.h"
#define malloc host_malloc
#define free host_free
#endif

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "encoder/impl/esp_opus_enc.h"
#include "decoder/impl/esp_opus_dec.h"

#include "mars_media.h"
#include "mars_resample.h"
#include "mars_audio_stream_core.h"

static const char *TAG = "mars_audio_stream";

/* P1-5：可触发的内存快照日志点。固件真实统计由 Codex 烧录后采集；
 * host 测试不编译 heap_caps，仅保留空实现以保证链接。 */
#if defined(MARS_HOST_TEST)
static void mem_snapshot(const char *stage) { (void)stage; }
static unsigned task_stack_hwm(void) { return 0; }
static int create_codec_task(TaskFunction_t fn, const char *name,
                             uint32_t stack, int priority,
                             TaskHandle_t *handle)
{
    return xTaskCreate(fn, name, stack, NULL, priority, handle);
}
static void delete_codec_task(void) { vTaskDelete(NULL); }
#else
#include "esp_heap_caps.h"
#include "freertos/idf_additions.h"
static void mem_snapshot(const char *stage)
{
    ESP_LOGI(TAG, "MEM[%s]: intl free=%u min=%u largest=%u | psram free=%u largest=%u",
             stage,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM));
}
static unsigned task_stack_hwm(void)
{
    return (unsigned)uxTaskGetStackHighWaterMark(NULL);
}
static int create_codec_task(TaskFunction_t fn, const char *name,
                             uint32_t stack, int priority,
                             TaskHandle_t *handle)
{
    /* 大栈放 PSRAM，TCB 仍由 IDF 留在 internal RAM。 */
    return xTaskCreateWithCaps(fn, name, stack, NULL, priority, handle,
                               MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}
static void delete_codec_task(void) { vTaskDeleteWithCaps(NULL); }
#endif

/* Espressif 的 Opus 集成测试给编码链 40 KiB、解码任务 20 KiB 栈。
 * 这里也按该上限配置，避免 Opus/SILK 的深调用栈破坏相邻 FreeRTOS 对象。 */
#define ENC_TASK_STACK     (40 * 1024)
#define DEC_TASK_STACK     (20 * 1024)
#define ENC_TASK_PRIO      5
#define DEC_TASK_PRIO      5

#define PKT_SLOT_SIZE      (MARS_PKT_HDR_SIZE + 7 + 2048)  /* 4B 头 + 7B 下行元信息 + 2KB 负载 */
#define PKT_QUEUE_DEPTH_DEF 8

/* 解码输出最大样本数：48kHz×60ms = 2880（覆盖 8k/12k/16k/24k/48k） */
#define DEC_MAX_SAMPLES    2880
/* 重采样输出上限：输入样本数（48k→16k 输出更小），够用 */
#define RS_MAX_SAMPLES     (DEC_MAX_SAMPLES + 4)

/* ═══════════ 生命周期锁（P0-2） ═══════════
 * 独立于队列锁：串行化 start/stop/play 首次初始化/playback_stop，
 * 保证同一时刻最多一个编码任务和一个解码任务。任务退出路径不依赖本锁，
 * 故 stop() 持锁等待任务退出不会死锁（任务只取队列/拼帧锁，不取本锁）。 */
static SemaphoreHandle_t s_lifecycle_mutex;

/* ═══════════ 上行状态 ═══════════ */

static TaskHandle_t        s_enc_task;
static atomic_bool          s_up_running;     /* lifecycle 锁下改写，公开查询原子读取 */
static atomic_bool          s_up_drain_req;
static bool                 s_up_finished; /* lifecycle lock; queue retained until stop */
static atomic_bool          s_up_stop_req;    /* release/acquire 跨核停止请求 */
static SemaphoreHandle_t   s_enc_exited;      /* 编码任务退出后 give（正式同步屏障） */
static SemaphoreHandle_t   s_acc_mutex;    /* 拼帧器（record 回调 vs 编码任务） */
static SemaphoreHandle_t   s_acc_sem;      /* 有新 PCM */
static SemaphoreHandle_t   s_up_q_mutex;   /* 上行队列（编码任务 vs 应用 read） */
static mars_frame_acc_t    s_acc;          /* 80ms → 60ms 拼帧 */
static mars_pkt_queue_t    s_up_q;
static uint8_t            *s_up_q_mem;     /* 队列槽位：普通 malloc，internal/PSRAM 由分配器决定 */
static bool                s_up_q_valid;   /* 队列内存是否存活（s_up_q_mutex 保护，read/stop 一致锁序） */
static void               *s_opus_enc;
static int                  s_enc_frame_in;    /* esp_opus_enc 输入帧样本数 */
static int                  s_enc_out_cap;     /* esp_opus_enc 输出缓冲上限 */
static uint8_t            *s_enc_outbuf;       /* 编码输出缓冲 */
static uint32_t             s_up_seq;           /* 上行包全局序号（槽内存 16 位低半） */
/* read 侧重建 32 位序号（ABI seq 为 uint64；单消费者轮询模型） */
static uint32_t             s_read_seq_hi;
static uint16_t             s_read_seq_last;

/* ═══════════ 下行状态 ═══════════ */

static TaskHandle_t        s_dec_task;
static bool                 s_dec_running;     /* 仅在 s_lifecycle_mutex 下改写 */
static atomic_bool          s_dec_stop_req;     /* release/acquire 跨核停止请求 */
static SemaphoreHandle_t   s_dec_exited;      /* 解码任务退出后 give（正式同步屏障） */
static SemaphoreHandle_t   s_down_sem;       /* 有新包 */
static SemaphoreHandle_t   s_down_q_mutex;   /* 下行队列（应用 push vs 解码任务 pop） */
static mars_pkt_queue_t    s_down_q;
static uint8_t            *s_down_q_mem;     /* 队列槽位：普通 malloc，internal/PSRAM 由分配器决定 */
static bool                s_down_q_valid;   /* 队列内存是否存活（s_down_q_mutex 保护） */
static atomic_bool         s_dec_busy;
static void               *s_opus_dec;
static uint32_t            s_dec_sample_rate;/* 当前解码器输出采样率 */
static uint32_t            s_down_seq;

/* ═══════════ 统计 ═══════════ */

typedef struct {
    atomic_uint_least32_t enc_packets;
    atomic_uint_least32_t dec_packets;
    atomic_uint_least32_t up_dropped_packets;
    atomic_uint_least32_t down_dropped_packets;
    atomic_uint_least32_t dropped_samples;
    atomic_uint_least32_t play_failures;
    atomic_uint_least32_t encode_failures;
    atomic_uint_least32_t decode_failures;
    atomic_uint_least32_t resample_failures;
} mars_audio_stream_atomic_stats_t;

static mars_audio_stream_atomic_stats_t s_stats;

/* One atomic word per direction keeps amplitude and sample age consistent.
 * 22 tick bits wrap safely; expiry is only 250ms. No callback into an app. */
static atomic_uint s_record_level, s_play_level;
static void update_level(atomic_uint *slot, const int16_t *pcm, size_t n)
{
    uint32_t total = 0, count = 0;
    if (!pcm || !n) return;
    for (size_t i = 0; i < n; i += 8) {
        int v = pcm[i];
        total += (uint32_t)(v < 0 ? -v : v);
        ++count;
    }
    uint32_t level = total / count / 16;
    if (level > 1000) level = 1000;
    uint32_t tick = (uint32_t)xTaskGetTickCount() & 0x3fffff;
    atomic_store(slot, (tick << 10) | level);
}
static uint16_t recent_level(atomic_uint *slot)
{
    uint32_t sample = atomic_load(slot);
    uint32_t age = ((uint32_t)xTaskGetTickCount() - (sample >> 10)) & 0x3fffff;
    return age <= pdMS_TO_TICKS(250) ? sample & 1023 : 0;
}
void mars_audio_levels(uint16_t *recording, uint16_t *playback)
{
    if (recording) *recording = recent_level(&s_record_level);
    if (playback) *playback = recent_level(&s_play_level);
}

#define STATS_ADD(field, value) \
    ((void)atomic_fetch_add_explicit(&s_stats.field, (uint32_t)(value), memory_order_relaxed))
#define STATS_SET(field, value) \
    atomic_store_explicit(&s_stats.field, (uint32_t)(value), memory_order_relaxed)
#define STATS_GET(field) \
    atomic_load_explicit(&s_stats.field, memory_order_relaxed)

static bool up_stop_requested(void)
{
    return atomic_load_explicit(&s_up_stop_req, memory_order_acquire);
}

static bool down_stop_requested(void)
{
    return atomic_load_explicit(&s_dec_stop_req, memory_order_acquire);
}

/* 仅复位上行统计（启动上行时调用，绝不动仍在运行的下行）。 */
static void stats_reset_up(void)
{
    STATS_SET(enc_packets, 0);
    STATS_SET(encode_failures, 0);
    STATS_SET(dropped_samples, 0);
    STATS_SET(up_dropped_packets, 0);
}
/* 仅复位下行统计（首次建解码流水线时调用，绝不动仍在运行的上行）。 */
static void stats_reset_down(void)
{
    STATS_SET(dec_packets, 0);
    STATS_SET(play_failures, 0);
    STATS_SET(decode_failures, 0);
    STATS_SET(resample_failures, 0);
    STATS_SET(down_dropped_packets, 0);
}

/* ═══════════ 上行：录音回调（mars_media 读任务上下文） ═══════════ */

static void record_cb(const int16_t *pcm, size_t samples, void *ctx)
{
    (void)ctx;
    update_level(&s_record_level, pcm, samples);
    /* 只做累积 + 通知，编码在独立任务（回调里禁止重活/阻塞） */
    if (xSemaphoreTake(s_acc_mutex, 0) == pdTRUE) {
        size_t dropped = 0;
        mars_frame_acc_push(&s_acc, pcm, samples, &dropped);
        /* P1-1：拼帧器溢出单位是 PCM 样本，计入 dropped_samples（不是包）。 */
        STATS_ADD(dropped_samples, dropped);
        xSemaphoreGive(s_acc_mutex);
        xSemaphoreGive(s_acc_sem);
    } else {
        /* 编码任务正持有锁：丢弃本帧并计数（不应发生，防御） */
        STATS_ADD(encode_failures, 1);
    }
}

/* ═══════════ 上行：编码任务 ═══════════ */

static void encode_task(void *arg)
{
    (void)arg;
    int16_t *frame = NULL;
    while (1) {
        if (frame == NULL) {
            frame = malloc(MARS_AUDIO_STREAM_FRAME_SAMPLES * sizeof(int16_t));
            if (frame == NULL) {
                /* 分配失败：重试直到成功或收到停止请求（不静默退出） */
                STATS_ADD(encode_failures, 1);
                vTaskDelay(pdMS_TO_TICKS(50));
                if (up_stop_requested()) {
                    break;
                }
                continue;
            }
        }
        /* 等待新 PCM（50ms 超时用于检查停止请求） */
        if (xSemaphoreTake(s_acc_sem, pdMS_TO_TICKS(50)) != pdTRUE) {
            if (up_stop_requested()) {
                break;
            }
            continue;
        }
        if (up_stop_requested() && !atomic_load(&s_up_drain_req)) {
            break;
        }
        /* 一次唤醒取满所有完整 60ms 帧（P0-1：每 80ms 回调必须产出 1.33 帧，
         * 不能只编码一帧）；锁只保护 pull 瞬间，编码在锁外执行。 */
        while (!up_stop_requested() || atomic_load(&s_up_drain_req)) {
            int got = 0;
            if (xSemaphoreTake(s_acc_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
                got = mars_frame_acc_pull(&s_acc, frame);
                xSemaphoreGive(s_acc_mutex);
            }
            if (!got) {
                break;
            }
            /* Opus 编码 60ms@16k（锁外） */
            esp_audio_enc_in_frame_t in = {
                .buffer = (uint8_t *)frame,
                .len    = (uint32_t)(MARS_AUDIO_STREAM_FRAME_SAMPLES * sizeof(int16_t)),
            };
            esp_audio_enc_out_frame_t out = {
                .buffer = s_enc_outbuf,
                .len    = (uint32_t)s_enc_out_cap,
                .encoded_bytes = 0,
            };
            esp_audio_err_t ret = esp_opus_enc_process(s_opus_enc, &in, &out);
            if (ret != ESP_AUDIO_ERR_OK || out.encoded_bytes == 0 ||
                out.encoded_bytes > (uint32_t)s_enc_out_cap) {
                STATS_ADD(encode_failures, 1);
                ESP_LOGW(TAG, "enc process failed: %d", (int)ret);
                continue;
            }
            uint16_t seq16 = (uint16_t)(++s_up_seq & 0xFFFF);
            if (xSemaphoreTake(s_up_q_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
                if (mars_pkt_queue_push(&s_up_q, out.buffer, out.encoded_bytes, seq16) != 0) {
                    STATS_ADD(encode_failures, 1);
                }
                xSemaphoreGive(s_up_q_mutex);
            } else {
                STATS_ADD(encode_failures, 1);
            }
            STATS_ADD(enc_packets, 1);
        }
        if (up_stop_requested()) break;
    }
    free(frame);
    ESP_LOGI(TAG, "enc task exit (stack_hwm=%u)",
             task_stack_hwm());
    /* P0-2：正式退出屏障——give 退出信号量，stop() 据此确认任务已完全返回，
     * 不再轮询 task handle。 */
    if (s_enc_exited != NULL) {
        xSemaphoreGive(s_enc_exited);
    }
    delete_codec_task();
}

/* ═══════════ 下行：解码任务 ═══════════ */

static void decode_task(void *arg)
{
    (void)arg;
    uint8_t *pkt = NULL;
    int16_t *pcm = NULL;
    int16_t *rs  = NULL;
    mars_voice_effect_t effect_state = {0};
    while (1) {
        if (pkt == NULL || pcm == NULL || rs == NULL) {
            if (pkt == NULL) {
                pkt = malloc(PKT_SLOT_SIZE);
            }
            if (pcm == NULL) {
                pcm = malloc(DEC_MAX_SAMPLES * sizeof(int16_t));
            }
            if (rs == NULL) {
                rs = malloc(RS_MAX_SAMPLES * sizeof(int16_t));
            }
            if (pkt == NULL || pcm == NULL || rs == NULL) {
                /* 分配失败：重试直到成功或收到停止请求（不静默退出） */
                STATS_ADD(decode_failures, 1);
                vTaskDelay(pdMS_TO_TICKS(50));
                if (down_stop_requested()) {
                    break;
                }
                continue;
            }
        }
        mars_resample_t res;

    while (1) {
        atomic_store(&s_dec_busy, false);
        if (xSemaphoreTake(s_down_sem, pdMS_TO_TICKS(50)) != pdTRUE) {
            if (down_stop_requested()) {
                break;
            }
            continue;
        }
        if (down_stop_requested()) {
            break;
        }
        size_t pkt_len = 0;
        uint32_t pkt_sr = 16000;
        uint16_t pkt_fms = 0;
        uint8_t pkt_effect = 0;
        {
            if (xSemaphoreTake(s_down_q_mutex, pdMS_TO_TICKS(10)) != pdTRUE) {
                /* 队列锁竞争：归还信号，稍后重试（避免丢包） */
                xSemaphoreGive(s_down_sem);
                continue;
            }
            int rc = mars_pkt_queue_pop(&s_down_q, pkt, PKT_SLOT_SIZE, &pkt_len, NULL);
            if (rc == 0) atomic_store(&s_dec_busy, true);
            xSemaphoreGive(s_down_q_mutex);
            if (rc != 0) {
                continue;
            }
            /* 负载前 7 字节 = 元信息：sr(4B) + fms(2B) + effect(1B)；其后为 Opus 包 */
            if (pkt_len < 7) {
                STATS_ADD(decode_failures, 1);
                continue;
            }
            pkt_sr = (uint32_t)pkt[0] | ((uint32_t)pkt[1] << 8) |
                     ((uint32_t)pkt[2] << 16) | ((uint32_t)pkt[3] << 24);
            pkt_fms = (uint16_t)pkt[4] | ((uint16_t)pkt[5] << 8);
            pkt_effect = pkt[6];
            memmove(pkt, pkt + 7, pkt_len - 7);
            pkt_len -= 7;
        }
        /* 解码器按包的采样率配置（变化时重建；解码器输出 rate = 配置 rate） */
        if (s_opus_dec == NULL || s_dec_sample_rate != pkt_sr) {
            if (s_opus_dec != NULL) {
                esp_opus_dec_close(s_opus_dec);
                s_opus_dec = NULL;
            }
            esp_opus_dec_frame_duration_t dur = ESP_OPUS_DEC_FRAME_DURATION_INVALID;
            if (pkt_fms == 20) {
                dur = ESP_OPUS_DEC_FRAME_DURATION_20_MS;
            } else if (pkt_fms == 40) {
                dur = ESP_OPUS_DEC_FRAME_DURATION_40_MS;
            } else if (pkt_fms == 60) {
                dur = ESP_OPUS_DEC_FRAME_DURATION_60_MS;
            }
            esp_opus_dec_cfg_t cfg = {
                .sample_rate    = pkt_sr,
                .channel        = 1,
                .frame_duration = dur,
                .self_delimited = false,
            };
            if (esp_opus_dec_open(&cfg, sizeof(cfg), &s_opus_dec) != ESP_AUDIO_ERR_OK ||
                s_opus_dec == NULL) {
                STATS_ADD(decode_failures, 1);
                s_opus_dec = NULL;
                continue;
            }
            s_dec_sample_rate = pkt_sr;
            ESP_LOGI(TAG, "dec opened @%uHz", (unsigned)pkt_sr);
        }
        esp_audio_dec_in_raw_t raw = {
            .buffer = pkt,
            .len    = (uint32_t)pkt_len,
            .consumed = 0,
            .frame_recover = ESP_AUDIO_DEC_RECOVERY_NONE,
        };
        esp_audio_dec_out_frame_t out = {
            .buffer = (uint8_t *)pcm,
            .len    = (uint32_t)(DEC_MAX_SAMPLES * sizeof(int16_t)),
            .needed_size = 0,
            .decoded_size = 0,
        };
        esp_audio_dec_info_t info = { 0 };
        esp_audio_err_t ret = esp_opus_dec_decode(s_opus_dec, &raw, &out, &info);
        if (ret != ESP_AUDIO_ERR_OK) {
            STATS_ADD(decode_failures, 1);
            if (ret == ESP_AUDIO_ERR_BUFF_NOT_ENOUGH) {
                ESP_LOGW(TAG, "dec decode buff too small (needed %u > %u)",
                         (unsigned)out.needed_size, (unsigned)out.len);
            } else {
                ESP_LOGW(TAG, "dec decode failed: %d", (int)ret);
            }
            continue;
        }
        size_t pcm_n = out.decoded_size / sizeof(int16_t);
        if (pcm_n == 0 || pcm_n > DEC_MAX_SAMPLES) {
            STATS_ADD(decode_failures, 1);
            continue;
        }
        STATS_ADD(dec_packets, 1);
                const uint32_t sr = (info.sample_rate != 0) ? info.sample_rate : pkt_sr;
        if (sr != MARS_AUDIO_STREAM_SAMPLE_RATE) {
            mars_resample_init(&res, sr, MARS_AUDIO_STREAM_SAMPLE_RATE);
            size_t rs_n = mars_resample_process(&res, pcm, pcm_n, rs, RS_MAX_SAMPLES);
            if (rs_n == 0) {
                STATS_ADD(resample_failures, 1);
                continue;
            }
            mars_voice_effect_process(&effect_state, rs, rs_n, pkt_effect);
            update_level(&s_play_level, rs, rs_n);
            if (mars_media_play(rs, rs_n, 0) != ESP_OK) {
                STATS_ADD(play_failures, 1);
            }
        } else {
            mars_voice_effect_process(&effect_state, pcm, pcm_n, pkt_effect);
            update_level(&s_play_level, pcm, pcm_n);
            if (mars_media_play(pcm, pcm_n, 0) != ESP_OK) {
                STATS_ADD(play_failures, 1);
            }
        }
    }   /* 内层消费循环 */
    break;   /* 内层因停止请求退出 → 跳出外层 */
    }   /* 外层 while（分配重试） */
    atomic_store(&s_dec_busy, false);
    if (s_opus_dec != NULL) {
        esp_opus_dec_close(s_opus_dec);
        s_opus_dec = NULL;
    }
    free(pkt);
    free(pcm);
    free(rs);
    ESP_LOGI(TAG, "dec task exit (stack_hwm=%u)",
             task_stack_hwm());
    /* P0-2：正式退出屏障 */
    if (s_dec_exited != NULL) {
        xSemaphoreGive(s_dec_exited);
    }
    delete_codec_task();
}

/* 初始化/清理（上下行各自独立，失败回滚）
 *
 * 注意：mutex/信号量对象跨启停复用（不随 stop 删除），保证 read/play
 * 与 stop 并发时不会操作已删除的句柄；stop 只释放大块内存与编解码器。
 *
 * P0-3：同步对象逐个创建；任一创建失败时删除本轮已建对象并把全部句柄
 * 复位为 NULL，下次 start 可整体重建（不能像旧版那样"第一个成功就跳过
 * 后续创建"，导致句柄半残且永远无法恢复）。 */

static void up_deinit(void)
{
    atomic_store(&s_record_level, 0);
    if (s_opus_enc != NULL) {
        esp_opus_enc_close(s_opus_enc);
        s_opus_enc = NULL;
    }
    free(s_enc_outbuf);
    s_enc_outbuf = NULL;
    s_enc_out_cap = 0;
    s_enc_frame_in = 0;
    free(s_up_q_mem);
    s_up_q_mem = NULL;
}

/* 创建上行同步原语（拼帧互斥/计数/队列互斥/退出信号量）；任一失败整体回滚。 */
static int up_sync_create(void)
{
    if (s_acc_mutex == NULL)    s_acc_mutex = xSemaphoreCreateMutex();
    if (s_acc_sem == NULL)       s_acc_sem = xSemaphoreCreateCounting(8, 0);
    if (s_up_q_mutex == NULL)    s_up_q_mutex = xSemaphoreCreateMutex();
    if (s_enc_exited == NULL)    s_enc_exited = xSemaphoreCreateBinary();
    if (s_acc_mutex == NULL || s_acc_sem == NULL || s_up_q_mutex == NULL ||
        s_enc_exited == NULL) {
        if (s_acc_mutex != NULL)    { vSemaphoreDelete(s_acc_mutex);    s_acc_mutex = NULL; }
        if (s_acc_sem != NULL)      { vSemaphoreDelete(s_acc_sem);      s_acc_sem = NULL; }
        if (s_up_q_mutex != NULL)   { vSemaphoreDelete(s_up_q_mutex);   s_up_q_mutex = NULL; }
        if (s_enc_exited != NULL)   { vSemaphoreDelete(s_enc_exited);  s_enc_exited = NULL; }
        return -12;   /* ENOMEM */
    }
    return 0;
}

static int up_init(const mars_audio_stream_config_t *cfg)
{
    uint16_t depth = (cfg->queue_depth >= 1 && cfg->queue_depth <= MARS_PKT_QUEUE_MAX_DEPTH)
                         ? (uint16_t)cfg->queue_depth : PKT_QUEUE_DEPTH_DEF;

    /* 同步原语跨启停复用（首次创建，失败整体回滚） */
    if (up_sync_create() != 0) {
        return -12;
    }
    /* 先建编码器（拿输入/输出尺寸），再按尺寸分配输出缓冲 */
    if (s_opus_enc == NULL) {
        esp_opus_enc_config_t cfg_enc = {
            .sample_rate      = cfg->sample_rate,
            .channel          = cfg->channels,
            .bits_per_sample  = 16,
            .bitrate          = (cfg->bitrate != 0) ? (int)cfg->bitrate : ESP_OPUS_BITRATE_AUTO,
            .frame_duration   = ESP_OPUS_ENC_FRAME_DURATION_60_MS,
            .application_mode = ESP_OPUS_ENC_APPLICATION_VOIP,
            .complexity       = (cfg->complexity >= 0 && cfg->complexity <= 10)
                                    ? cfg->complexity : 5,
            .enable_fec       = false,
            .enable_dtx       = false,
            .enable_vbr       = true,
        };
        if (esp_opus_enc_open(&cfg_enc, sizeof(cfg_enc), &s_opus_enc) != ESP_AUDIO_ERR_OK ||
            s_opus_enc == NULL) {
            ESP_LOGE(TAG, "start: opus enc open failed");
            up_deinit();
            return -12;
        }
        int in_sz = 0, out_sz = 0;
        const int expected_in_bytes =
            MARS_AUDIO_STREAM_FRAME_SAMPLES * (int)sizeof(int16_t);
        if (esp_opus_enc_get_frame_size(s_opus_enc, &in_sz, &out_sz) != ESP_AUDIO_ERR_OK ||
            in_sz != expected_in_bytes || out_sz <= 0 || out_sz > 2048) {
            ESP_LOGE(TAG, "start: 编码器帧 %dB/%dB != 期望输入 %dB（驱动不匹配）",
                     in_sz, out_sz, expected_in_bytes);
            up_deinit();
            return -5;
        }
        s_enc_frame_in = in_sz;
        s_enc_out_cap  = out_sz;
        ESP_LOGI(TAG, "enc frame_in=%d out_cap=%d", s_enc_frame_in, s_enc_out_cap);
    }
    if (s_up_q_mem == NULL) {
        s_up_q_mem = malloc((size_t)depth * PKT_SLOT_SIZE);
        if (s_up_q_mem == NULL) {
            up_deinit();
            return -12;
        }
    }
    if (s_enc_outbuf == NULL) {
        s_enc_outbuf = malloc((size_t)s_enc_out_cap);
        if (s_enc_outbuf == NULL) {
            up_deinit();
            return -12;
        }
    }
    mars_frame_acc_init(&s_acc, MARS_AUDIO_STREAM_FRAME_SAMPLES);
    mars_pkt_queue_init(&s_up_q, s_up_q_mem, PKT_SLOT_SIZE, depth);
    return 0;
}

static void down_deinit(void)
{
    atomic_store(&s_play_level, 0);
    /* 同步原语跨启停复用（首次创建），stop 只释放队列内存 */
    free(s_down_q_mem);
    s_down_q_mem = NULL;
}

/* 创建下行同步原语（计数/队列互斥/退出信号量）；任一失败整体回滚并复位。 */
static int down_sync_create(void)
{
    if (s_down_sem == NULL)     s_down_sem = xSemaphoreCreateCounting(8, 0);
    if (s_down_q_mutex == NULL) s_down_q_mutex = xSemaphoreCreateMutex();
    if (s_dec_exited == NULL)   s_dec_exited = xSemaphoreCreateBinary();
    if (s_down_sem == NULL || s_down_q_mutex == NULL || s_dec_exited == NULL) {
        if (s_down_sem != NULL)     { vSemaphoreDelete(s_down_sem);     s_down_sem = NULL; }
        if (s_down_q_mutex != NULL) { vSemaphoreDelete(s_down_q_mutex); s_down_q_mutex = NULL; }
        if (s_dec_exited != NULL)   { vSemaphoreDelete(s_dec_exited);   s_dec_exited = NULL; }
        return -12;
    }
    return 0;
}

static int down_init(void)
{
    if (down_sync_create() != 0) {
        return -12;
    }
    if (s_down_q_mem == NULL) {
        s_down_q_mem = malloc((size_t)PKT_QUEUE_DEPTH_DEF * PKT_SLOT_SIZE);
        if (s_down_q_mem == NULL) {
            down_deinit();
            return -12;
        }
    }
    mars_pkt_queue_init(&s_down_q, s_down_q_mem, PKT_SLOT_SIZE, PKT_QUEUE_DEPTH_DEF);
    return 0;
}

/* ═══════════ 模块初始化（系统启动时调用一次，单线程） ═══════════
 * 创建生命周期锁。必须在任何动态应用/并发调用之前由 app_main 调用，
 * 否则首次并发 start/play 会重复创建该锁。 */
void mars_audio_stream_init(void)
{
    if (s_lifecycle_mutex == NULL) {
        s_lifecycle_mutex = xSemaphoreCreateMutex();
    }
}

/* ═══════════ 公开接口：上行 ═══════════ */

int mars_audio_stream_start(const mars_audio_stream_config_t *cfg)
{
    mars_audio_stream_config_t def = MARS_AUDIO_STREAM_CONFIG_DEFAULT();
    if (cfg == NULL) {
        cfg = &def;
    }
    if (s_lifecycle_mutex == NULL) {
        return -12;
    }
    xSemaphoreTake(s_lifecycle_mutex, portMAX_DELAY);
    if (s_up_finished || atomic_load(&s_up_drain_req)) {
        xSemaphoreGive(s_lifecycle_mutex); return -16;
    }
    if (atomic_load_explicit(&s_up_running, memory_order_acquire)) {
        if (up_stop_requested()) {
            xSemaphoreGive(s_lifecycle_mutex);
            return -16;   /* 正在停止，不能把半关闭状态当作启动成功 */
        }
        xSemaphoreGive(s_lifecycle_mutex);
        return 0;   /* 幂等 */
    }
    if (cfg->sample_rate != MARS_AUDIO_STREAM_SAMPLE_RATE ||
        cfg->frame_ms    != MARS_AUDIO_STREAM_FRAME_MS ||
        cfg->channels    != MARS_AUDIO_STREAM_CHANNELS) {
        ESP_LOGE(TAG, "start: 仅支持 %uHz/%ums/单声道（got %uHz/%ums/ch=%u）",
                 MARS_AUDIO_STREAM_SAMPLE_RATE, MARS_AUDIO_STREAM_FRAME_MS,
                 (unsigned)cfg->sample_rate, (unsigned)cfg->frame_ms,
                 (unsigned)cfg->channels);
        xSemaphoreGive(s_lifecycle_mutex);
        return -22;   /* EINVAL */
    }
    if (!mars_media_is_ready()) {
        ESP_LOGE(TAG, "start: 音频底层未就绪");
        xSemaphoreGive(s_lifecycle_mutex);
        return -19;   /* ENODEV */
    }
    if (mars_media_record_is_active()) {
        ESP_LOGE(TAG, "start: 底层录音已被占用（旧录音 API 互斥）");
        xSemaphoreGive(s_lifecycle_mutex);
        return -16;   /* EBUSY */
    }
    int rc = up_init(cfg);
    if (rc != 0) {
        xSemaphoreGive(s_lifecycle_mutex);
        return rc;
    }
    atomic_store_explicit(&s_up_stop_req, false, memory_order_release);
    s_up_seq = 0;
    s_read_seq_hi = 0;
    s_read_seq_last = 0;
    stats_reset_up();   /* P1-1：只复位上行统计，不清仍在运行的下行 */
    s_enc_task = NULL;
    /* 清除上一次“stop 超时、任务随后退出”可能遗留的退出 token。 */
    while (xSemaphoreTake(s_enc_exited, 0) == pdTRUE) {
    }

    esp_err_t err = mars_media_record_start(record_cb, NULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "start: mars_media_record_start failed: %d", (int)err);
        up_deinit();
        xSemaphoreGive(s_lifecycle_mutex);
        return -(int)err;
    }
    if (create_codec_task(encode_task, "mars_enc", ENC_TASK_STACK, ENC_TASK_PRIO,
                          &s_enc_task) != pdPASS) {
        mars_media_record_stop();
        up_deinit();
        xSemaphoreGive(s_lifecycle_mutex);
        return -12;
    }
    /* 队列内存就绪：在队列锁下置 valid，read() 据此与 stop() 建立一致锁序。 */
    xSemaphoreTake(s_up_q_mutex, portMAX_DELAY);
    s_up_q_valid = true;
    xSemaphoreGive(s_up_q_mutex);
    atomic_store_explicit(&s_up_running, true, memory_order_release);
    xSemaphoreGive(s_lifecycle_mutex);
    mem_snapshot("up-started");
    ESP_LOGI(TAG, "stream started (16k/60ms, queue=%d)", PKT_QUEUE_DEPTH_DEF);
    return 0;
}

int mars_audio_stream_read(uint8_t *dst, size_t capacity,
                           mars_audio_packet_info_t *info, uint32_t timeout_ms)
{
    if (dst == NULL || capacity == 0) {
        return -22;
    }
    if (s_up_q_mutex == NULL) {
        return -110;
    }
    /* 在队列锁内检查队列是否存活：与 stop() 的 reset/free 建立一致锁序，
     * 绝不 pop 正在释放的队列内存（P0-2）。 */
    size_t len = 0;
    uint16_t seq = 0;
    int rc = -1;
    int q_valid = 0;
    if (xSemaphoreTake(s_up_q_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
        q_valid = s_up_q_valid;
        if (q_valid) {
            rc = mars_pkt_queue_pop(&s_up_q, dst, capacity, &len, &seq);
        }
        xSemaphoreGive(s_up_q_mutex);
    }
    if (!q_valid) {
        return -110;   /* 上行未运行/正在停止 */
    }
    if (rc == -2) {
        return -12;   /* 缓冲过小且包未消费 */
    }
    if (rc == -1) {
        /* 队列空：轮询等待（不消费信号量，避免与编码任务竞争） */
        if (timeout_ms == 0) {
            return -11;   /* EAGAIN */
        }
        TickType_t start_t = xTaskGetTickCount();
        TickType_t deadline = start_t + pdMS_TO_TICKS(timeout_ms);
        do {
            vTaskDelay(pdMS_TO_TICKS(5));
            rc = -1;
            q_valid = 0;
            if (xSemaphoreTake(s_up_q_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
                q_valid = s_up_q_valid;
                if (q_valid) {
                    rc = mars_pkt_queue_pop(&s_up_q, dst, capacity, &len, &seq);
                }
                xSemaphoreGive(s_up_q_mutex);
            }
            if (!q_valid) {
                return -110;   /* 等待期间 stop() 释放了队列 */
            }
            if (rc == 0) {
                break;
            }
            if (rc == -2) {
                return -12;
            }
        } while ((int32_t)(deadline - xTaskGetTickCount()) > 0);
        if (rc != 0) {
            return -11;   /* 超时仍无包 */
        }
    }
    if (info != NULL) {
        memset(info, 0, sizeof(*info));
        info->sample_rate = MARS_AUDIO_STREAM_SAMPLE_RATE;
        info->frame_ms    = MARS_AUDIO_STREAM_FRAME_MS;
        info->channels    = MARS_AUDIO_STREAM_CHANNELS;
        /* 从 16 位槽序号重建 32 位全局序号（环绕检测） */
        if (s_read_seq_last > seq && (uint16_t)(s_read_seq_last - seq) > 0x8000u) {
            ++s_read_seq_hi;
        }
        s_read_seq_last = seq;
        info->seq = ((uint64_t)s_read_seq_hi << 16) | seq;
        info->payload_len = (uint32_t)len;
    }
    return 0;
}

/* Stop the producer and encoder while preserving the final encoded packets.
 * Caller drains read() then invokes stop() to free the queue/codec context. */
int mars_audio_stream_finish(void)
{
    if (!s_lifecycle_mutex) return 0;
    xSemaphoreTake(s_lifecycle_mutex, portMAX_DELAY);
    if (s_up_finished || (!atomic_load(&s_up_running) && s_enc_task == NULL)) {
        xSemaphoreGive(s_lifecycle_mutex); return 0;
    }
    if (up_stop_requested() && !atomic_load(&s_up_drain_req)) {
        xSemaphoreGive(s_lifecycle_mutex); return -16;
    }
    atomic_store(&s_up_drain_req, true);
    /* Quiesce the producer before requesting encoder exit. */
    esp_err_t err = mars_media_record_stop();
    if (err != ESP_OK) { xSemaphoreGive(s_lifecycle_mutex); return -(int)err; }
    if (!up_stop_requested()) {
        xSemaphoreTake(s_acc_mutex, portMAX_DELAY);
        /* Pad the last partial 60 ms frame with silence, preserving its tail. */
        size_t remainder = s_acc.n % s_acc.frame_samples;
        if (remainder) {
            size_t padding = s_acc.frame_samples - remainder;
            memset(s_acc.buf + s_acc.n, 0, padding * sizeof(int16_t));
            s_acc.n += padding;
        }
        xSemaphoreGive(s_acc_mutex);
        atomic_store_explicit(&s_up_stop_req, true, memory_order_release);
    }
    xSemaphoreGive(s_acc_sem);
    if (xSemaphoreTake(s_enc_exited, pdMS_TO_TICKS(1000)) != pdTRUE) {
        xSemaphoreGive(s_lifecycle_mutex); return -110;
    }
    s_enc_task = NULL; s_up_finished = true;
    atomic_store(&s_up_running, false);
    xSemaphoreGive(s_lifecycle_mutex);
    return 0;
}

int mars_audio_stream_stop(void)
{
    if (s_lifecycle_mutex == NULL) return 0;
    xSemaphoreTake(s_lifecycle_mutex, portMAX_DELAY);
    if (!atomic_load(&s_up_running) && s_enc_task == NULL && !s_up_finished) {
        xSemaphoreGive(s_lifecycle_mutex); return 0;
    }
    if (!s_up_finished) {
        atomic_store_explicit(&s_up_stop_req, true, memory_order_release);
        esp_err_t err = mars_media_record_stop();
        if (err != ESP_OK) {
            xSemaphoreGive(s_acc_sem);
            xSemaphoreGive(s_lifecycle_mutex); return -(int)err;
        }
        xSemaphoreGive(s_acc_sem);
        if (xSemaphoreTake(s_enc_exited, pdMS_TO_TICKS(1000)) != pdTRUE) {
            ESP_LOGE(TAG, "stop: enc task did not exit in 1s; retry required");
            xSemaphoreGive(s_lifecycle_mutex); return -110;
        }
    }
    xSemaphoreTake(s_up_q_mutex, portMAX_DELAY);
    STATS_SET(up_dropped_packets, mars_pkt_queue_dropped(&s_up_q));
    s_up_q_valid = false;
    mars_pkt_queue_reset(&s_up_q);
    xSemaphoreGive(s_up_q_mutex);
    s_enc_task = NULL; s_up_finished = false;
    up_deinit();
    atomic_store(&s_up_running, false);
    atomic_store(&s_up_stop_req, false);
    atomic_store(&s_up_drain_req, false);
    xSemaphoreGive(s_lifecycle_mutex);
    mem_snapshot("up-stopped");
    ESP_LOGI(TAG, "stream stopped (enc=%u dec=%u drop=%u play_fail=%u)",
             (unsigned)STATS_GET(enc_packets), (unsigned)STATS_GET(dec_packets),
             (unsigned)(STATS_GET(up_dropped_packets) + STATS_GET(down_dropped_packets)),
             (unsigned)STATS_GET(play_failures));
    return 0;
}

/* ═══════════ 公开接口：下行 ═══════════ */

int mars_audio_play_opus(const uint8_t *data, size_t len,
                         uint32_t sample_rate, uint16_t frame_ms)
{
    return mars_audio_play_opus_effect(data,len,sample_rate,frame_ms,MARS_VOICE_ORIGINAL);
}

int mars_audio_play_opus_effect(const uint8_t *data, size_t len,
                         uint32_t sample_rate, uint16_t frame_ms, uint8_t effect)
{
    if (data == NULL || len == 0) {
        return -22;
    }
    if (sample_rate != 8000 && sample_rate != 12000 && sample_rate != 16000 &&
        sample_rate != 24000 && sample_rate != 48000) {
        return -22;
    }
    if (effect >= MARS_VOICE_COUNT) return -22;
    if (frame_ms != 0 && frame_ms != 20 && frame_ms != 40 && frame_ms != 60) {
        return -22;
    }
    /* P0-3：包长前置校验，放到任何 init/claim/task 创建之前。
     * 槽内布局 = MARS_PKT_HDR_SIZE(4) + 元信息(7) + 负载；负载上限 =
     * PKT_SLOT_SIZE - MARS_PKT_HDR_SIZE - 7。防溢出写法（两边均为小整数）。 */
    const size_t max_payload = PKT_SLOT_SIZE - MARS_PKT_HDR_SIZE - 7;
    if (len > max_payload) {
        return -22;   /* EINVAL：单包超槽，此时尚未创建任何资源 */
    }
    if (s_lifecycle_mutex == NULL) {
        return -12;
    }
    /* 组装包：负载 = 元信息(6B: sr4+fms2) + Opus 包。先在锁内完成临时内存分配，
     * 这样首次 init 的任一点失败（队列/claim/任务创建/临时内存）都能干净回滚。 */
    size_t total = 7 + len;
    uint8_t meta[7];
    meta[0] = (uint8_t)(sample_rate & 0xFF);
    meta[1] = (uint8_t)((sample_rate >> 8) & 0xFF);
    meta[2] = (uint8_t)((sample_rate >> 16) & 0xFF);
    meta[3] = (uint8_t)((sample_rate >> 24) & 0xFF);
    meta[4] = (uint8_t)(frame_ms & 0xFF);
    meta[5] = (uint8_t)((frame_ms >> 8) & 0xFF);
    meta[6] = effect;

    xSemaphoreTake(s_lifecycle_mutex, portMAX_DELAY);
    if (s_dec_running && down_stop_requested()) {
        xSemaphoreGive(s_lifecycle_mutex);
        return -16;   /* 上一次 stop 超时，等待调用方重试 stop 完成收尾 */
    }
    uint8_t *tmp = malloc(total);
    if (tmp == NULL) {
        xSemaphoreGive(s_lifecycle_mutex);
        return -12;
    }
    memcpy(tmp, meta, 7);
    memcpy(tmp + 7, data, len);

    if (!s_dec_running && s_dec_task == NULL) {
        int rc = down_init();
        if (rc != 0) {
            free(tmp);
            xSemaphoreGive(s_lifecycle_mutex);
            return rc;
        }
        if (mars_media_output_claim() != ESP_OK) {
            ESP_LOGE(TAG, "play: 输出通路已被其他所有者占用");
            down_deinit();
            free(tmp);
            xSemaphoreGive(s_lifecycle_mutex);
            return -16;   /* EBUSY */
        }
        atomic_store_explicit(&s_dec_stop_req, false, memory_order_release);
        s_dec_sample_rate = 0;
        s_down_seq = 0;
        stats_reset_down();   /* P1-1：只复位下行统计，不清仍在运行的上行 */
        while (xSemaphoreTake(s_dec_exited, 0) == pdTRUE) {
        }
        s_dec_task = NULL;
        if (create_codec_task(decode_task, "mars_dec", DEC_TASK_STACK, DEC_TASK_PRIO,
                              &s_dec_task) != pdPASS) {
            mars_media_output_release();
            down_deinit();
            free(tmp);
            xSemaphoreGive(s_lifecycle_mutex);
            return -12;
        }
        xSemaphoreTake(s_down_q_mutex, portMAX_DELAY);
        s_down_q_valid = true;
        xSemaphoreGive(s_down_q_mutex);
        s_dec_running = true;
        mem_snapshot("down-started");
        ESP_LOGI(TAG, "playback started (dec task)");
    }
    uint16_t seq16 = (uint16_t)(++s_down_seq & 0xFFFF);
    int rc = -1;
    /* 首包已经创建流水线时不能因短暂锁竞争返回失败并遗留任务/claim。
     * lifecycle 锁保证提交方串行，解码任务只短持队列锁，因此这里阻塞取得。 */
    xSemaphoreTake(s_down_q_mutex, portMAX_DELAY);
    rc = mars_pkt_queue_push(&s_down_q, tmp, total, seq16);
    xSemaphoreGive(s_down_q_mutex);
    free(tmp);
    if (rc == 0) {
        xSemaphoreGive(s_down_sem);
    } else {
        STATS_ADD(play_failures, 1);
        rc = -12;
    }
    xSemaphoreGive(s_lifecycle_mutex);
    return rc;
}

int mars_audio_playback_pending(void)
{
    if (!s_lifecycle_mutex) return 0;
    xSemaphoreTake(s_lifecycle_mutex, portMAX_DELAY);
    int pending = 0;
    if (s_down_q_mutex) {
        xSemaphoreTake(s_down_q_mutex, portMAX_DELAY);
        if (s_down_q_valid) pending = mars_pkt_queue_count(&s_down_q) + (atomic_load(&s_dec_busy) ? 1 : 0);
        xSemaphoreGive(s_down_q_mutex);
    }
    xSemaphoreGive(s_lifecycle_mutex);
    return pending;
}

int mars_audio_playback_stop(void)
{
    if (s_lifecycle_mutex == NULL) {
        return 0;
    }
    xSemaphoreTake(s_lifecycle_mutex, portMAX_DELAY);
    if (!s_dec_running && s_dec_task == NULL) {
        xSemaphoreGive(s_lifecycle_mutex);
        return 0;
    }
    atomic_store_explicit(&s_dec_stop_req, true, memory_order_release);
    xSemaphoreGive(s_down_sem);
    /* 正式退出屏障：等解码任务 give s_dec_exited。 */
    if (xSemaphoreTake(s_dec_exited, pdMS_TO_TICKS(1000)) != pdTRUE) {
        ESP_LOGE(TAG, "playback_stop: dec task 未在 1s 内退出（保持停止请求，调用方需重试）");
        xSemaphoreGive(s_lifecycle_mutex);
        return -110;
    }
    /* 在队列锁保护下置 invalid、reset、释放队列内存（与解码任务 push/pop 一致锁序）。 */
    xSemaphoreTake(s_down_q_mutex, portMAX_DELAY);
    STATS_SET(down_dropped_packets, mars_pkt_queue_dropped(&s_down_q));
    s_down_q_valid = false;
    mars_pkt_queue_reset(&s_down_q);
    xSemaphoreGive(s_down_q_mutex);
    s_dec_task = NULL;
    if (s_opus_dec != NULL) {
        esp_opus_dec_close(s_opus_dec);
        s_opus_dec = NULL;
    }
    down_deinit();
    mars_media_output_release();
    s_dec_running = false;
    atomic_store_explicit(&s_dec_stop_req, false, memory_order_release);
    xSemaphoreGive(s_lifecycle_mutex);
    mem_snapshot("down-stopped");
    ESP_LOGI(TAG, "playback stopped");
    return 0;
}

/* ═══════════ 统计 ═══════════ */

void mars_audio_stream_stats(mars_audio_stream_stats_t *out)
{
    if (out == NULL) {
        return;
    }
    uint32_t up_drop = 0, down_drop = 0;
    if (s_up_q_mutex != NULL) {
        if (xSemaphoreTake(s_up_q_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
            up_drop = mars_pkt_queue_dropped(&s_up_q);
            xSemaphoreGive(s_up_q_mutex);
        }
    }
    if (s_down_q_mutex != NULL) {
        if (xSemaphoreTake(s_down_q_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
            down_drop = mars_pkt_queue_dropped(&s_down_q);
            xSemaphoreGive(s_down_q_mutex);
        }
    }
    out->enc_packets      = STATS_GET(enc_packets);
    out->dec_packets      = STATS_GET(dec_packets);
    out->dropped_packets  = STATS_GET(up_dropped_packets) + up_drop +
                            STATS_GET(down_dropped_packets) + down_drop;
    out->dropped_samples  = STATS_GET(dropped_samples);
    out->play_failures    = STATS_GET(play_failures);
    out->encode_failures  = STATS_GET(encode_failures);
    out->decode_failures  = STATS_GET(decode_failures);
    out->resample_failures = STATS_GET(resample_failures);
}

int mars_audio_stream_is_running(void)
{
    return atomic_load_explicit(&s_up_running, memory_order_acquire) ? 1 : 0;
}

#if defined(MARS_HOST_TEST)
/* 主机故障注入专用：回到"刚上电、同步原语尚未创建"的干净状态。
 * 同步原语在固件里跨启停复用，正常测试中途不会再创建；为了验证
 * semaphore/mutex 创建失败的回滚路径，host 测试用此钩子强制复位。
 * 仅在 MARS_HOST_TEST 下编译，不进入固件 ABI。 */
void mars_audio_stream_reset_for_test(void)
{
    mars_audio_stream_stop();
    mars_audio_playback_stop();
    if (s_acc_mutex != NULL)  { vSemaphoreDelete(s_acc_mutex);  s_acc_mutex = NULL; }
    if (s_acc_sem != NULL)    { vSemaphoreDelete(s_acc_sem);    s_acc_sem = NULL; }
    if (s_up_q_mutex != NULL) { vSemaphoreDelete(s_up_q_mutex); s_up_q_mutex = NULL; }
    if (s_enc_exited != NULL) { vSemaphoreDelete(s_enc_exited); s_enc_exited = NULL; }
    if (s_down_sem != NULL)   { vSemaphoreDelete(s_down_sem);   s_down_sem = NULL; }
    if (s_down_q_mutex != NULL){vSemaphoreDelete(s_down_q_mutex);s_down_q_mutex = NULL; }
    if (s_dec_exited != NULL)  { vSemaphoreDelete(s_dec_exited);  s_dec_exited = NULL; }
}

void mars_audio_stream_reset_lifecycle_for_test(void)
{
    mars_audio_stream_reset_for_test();
    if (s_lifecycle_mutex != NULL) {
        vSemaphoreDelete(s_lifecycle_mutex);
        s_lifecycle_mutex = NULL;
    }
}
#endif
