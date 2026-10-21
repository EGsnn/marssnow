/* P1 服务层 host 仿真测试
 *
 * 编译（stub 打桩，把 mars_audio_stream.c 拉进 x86 可执行）：
 *   clang -std=c11 -Wall -Wextra -Werror -DMARS_HOST_TEST \
 *     -I stub -I ../include \
 *     test_service.c stub/FreeRTOS_stub.c stub/media_stub.c stub/codec_stub.c \
 *     stub/host_malloc.c ../mars_audio_stream.c ../mars_audio_stream_core.c ../mars_resample.c \
 *     -lpthread -o test_service && ./test_service
 *
 * 覆盖（对应 Codex P0-1..4 / P1-6 / P1-9）：
 *   - 真实回调消费：100 次 80ms 回调 → 133 帧 60ms 编码（P0-1）
 *   - 上下行并发压力（P0-2）
 *   - 只播放、从未录音（P0-3）
 *   - 50 轮启停无泄漏（P0-4）
 *   - 分配失败注入与回滚（P0-4 / P1-9）
 *   - 错误码：-110/-11/-12 区分、逐包 seq、queue_depth 生效、双向 dropped（P1-6）
 */
#include <pthread.h>
#include <stdio.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "mars_audio_stream.h"
#include "mars_audio_stream_core.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "mars_media.h"
#include "encoder/impl/esp_opus_enc.h"
#include "decoder/impl/esp_opus_dec.h"
#include "host_malloc.h"

/* host 故障注入：把同步原语复位到"未创建"状态，以验证创建失败回滚（P0-3） */
extern void mars_audio_stream_reset_for_test(void);
extern void mars_audio_stream_reset_lifecycle_for_test(void);

static int g_fails = 0;

#define CHECK(cond, msg)                                                     \
    do {                                                                     \
        if (cond) {                                                          \
            printf("  PASS: %s\n", msg);                                     \
        } else {                                                             \
            printf("  FAIL: %s\n", msg);                                     \
            g_fails++;                                                       \
        }                                                                    \
    } while (0)

/* ── 读线程：持续 read 收集 seq，直到 stop_flag ── */
typedef struct {
    atomic_int stop;
    uint64_t first_seq;
    uint64_t last_seq;
    unsigned long packets;
    int seq_ok;      /* 严格 +1 连续 */
    int mono_ok;     /* 单调递增（缺口允许，回退/重复不允许） */
    uint64_t last;
    uint32_t timeout_ms;
    unsigned long seq_trace[8];
    int seq_trace_n;
} collector_t;

static void *collector_thread(void *arg)
{
    collector_t *c = arg;
    uint8_t buf[2048];
    mars_audio_packet_info_t info;
    c->packets = 0;
    c->first_seq = 0;
    c->seq_ok = 1;
    c->mono_ok = 1;
    c->last = 0;
    int dbg = 0;
    while (!atomic_load_explicit(&c->stop, memory_order_acquire)) {
        if (dbg < 4) { printf("  [c] before read\n"); }
        int rc = mars_audio_stream_read(buf, sizeof(buf), &info, c->timeout_ms);
        if (dbg < 4) { printf("  [c] after read rc=%d\n", rc); dbg++; }
        if (rc == 0) {
            if (c->seq_trace_n < 8) {
                c->seq_trace[c->seq_trace_n++] = info.seq;
            }
            if (c->packets == 0) {
                c->first_seq = info.seq;
            } else if (info.seq != c->last + 1) {
                c->seq_ok = 0;
            }
            if (info.seq <= c->last) {
                c->mono_ok = 0;
            }
            c->last = info.seq;
            c->last_seq = info.seq;
            c->packets++;
        } else if (rc != -11 && rc != -110) {
            printf("  [warn] read rc=%d\n", rc);
        }
    }
    /* 注意：不在此清空剩余包——队列清理由 stop() 完成（P0-4），
     * 避免编码任务持续产包导致清空循环永不退出。 */
    return NULL;
}

/* ── 下行 push 线程：连续 play_opus ── */
typedef struct {
    atomic_int stop;
    int count;
    int ok;
} pusher_t;

static void *pusher_thread(void *arg)
{
    pusher_t *p = arg;
    static uint8_t pkt[100];
    memset(pkt, 0x11, sizeof(pkt));
    p->count = 0;
    p->ok = 1;
    while (!atomic_load_explicit(&p->stop, memory_order_acquire)) {
        int rc = mars_audio_play_opus(pkt, sizeof(pkt), 16000, 60);
        if (rc != 0) {
            p->ok = 0;
        }
        p->count++;
    }
    return NULL;
}

/* ═══════ 1. 真实回调消费：100 回调 → 133 帧 ═══════ */
static void t_real_consume(void)
{
    printf("\n[t1] 真实回调消费 100×1280 → 133×960（P0-1）\n");
    stub_media_reset();
    stub_media_set_record_interval_ms(5);

    mars_audio_stream_config_t cfg = MARS_AUDIO_STREAM_CONFIG_DEFAULT();
    cfg.queue_depth = 8;
    CHECK(mars_audio_stream_start(&cfg) == 0, "start 返回 0");

    collector_t c = {0};
    c.timeout_ms = 20;
    pthread_t tid;
    int crc = pthread_create(&tid, NULL, collector_thread, &c);
    printf("  [t1] pthread_create rc=%d\n", crc);
    if (crc != 0) { g_fails++; mars_audio_stream_stop(); return; }
    usleep(500000);            /* ≈100 次回调（5ms × 100） */
    atomic_store_explicit(&c.stop, 1, memory_order_release);
    printf("  [t1] join collector...\n");
    pthread_join(tid, NULL);
    printf("  [t1] collector joined: pkts=%lu seq_ok=%d\n", c.packets, c.seq_ok);

    unsigned long calls = stub_media_record_calls();
    unsigned long expect = (unsigned long)(calls * 1280ul / 960ul);   /* 实际回调 → 满帧编码 */
    printf("  (record_calls=%lu → expect_enc=%lu, got=%lu, seq_head=[%lu %lu %lu %lu %lu])\n",
           calls, expect, c.packets,
           c.seq_trace_n > 0 ? c.seq_trace[0] : 0,
           c.seq_trace_n > 1 ? c.seq_trace[1] : 0,
           c.seq_trace_n > 2 ? c.seq_trace[2] : 0,
           c.seq_trace_n > 3 ? c.seq_trace[3] : 0,
           c.seq_trace_n > 4 ? c.seq_trace[4] : 0);
    CHECK(c.seq_ok, "seq 严格递增无缺口");
    CHECK(c.packets >= expect - 2, "收包数 ≥ 满帧数-2（stop 清队列瞬间 1-2 帧，容差 2）");
    CHECK(c.first_seq == 1, "首包 seq == 1");

    mars_audio_stream_stats_t st;
    mars_audio_stream_stats(&st);
    CHECK(st.enc_packets >= c.packets, "stats.enc_packets ≥ 已读取包数");
    CHECK(st.dropped_packets == 0, "无丢包（读线程消费及时）");

    CHECK(mars_audio_stream_stop() == 0, "stop 返回 0");
}

/* ═══════ 2. 并发压力：上行 编码push vs 读pop；下行 多push vs 解码pop ═══════ */
static void t_concurrent(void)
{
    printf("\n[t2] 上下行并发压力（P0-2）\n");
    stub_media_reset();
    stub_media_set_record_interval_ms(5);

    mars_audio_stream_config_t cfg = MARS_AUDIO_STREAM_CONFIG_DEFAULT();
    cfg.queue_depth = 8;
    CHECK(mars_audio_stream_start(&cfg) == 0, "start 返回 0");

    /* 上行读线程（5ms 回调：读线程忙循环可跟上，验证无撕裂） */
    collector_t c = {0};
    c.timeout_ms = 0;
    pthread_t rt;
    pthread_create(&rt, NULL, collector_thread, &c);

    /* 下行 2 个 push 线程 */
    pusher_t p1 = {0}, p2 = {0};
    pthread_t t1, t2;
    pthread_create(&t1, NULL, pusher_thread, &p1);
    pthread_create(&t2, NULL, pusher_thread, &p2);

    usleep(300000);
    atomic_store_explicit(&p1.stop, 1, memory_order_release);
    atomic_store_explicit(&p2.stop, 1, memory_order_release);
    pthread_join(t1, NULL);
    pthread_join(t2, NULL);
    atomic_store_explicit(&c.stop, 1, memory_order_release);
    pthread_join(rt, NULL);

    mars_audio_stream_stats_t st;
    mars_audio_stream_stats(&st);
    printf("  up_pkts=%lu down_pkts=%u down_drop=%u\n",
           c.packets, st.dec_packets, st.dropped_packets);
    CHECK(c.mono_ok, "上行 seq 单调无回退/重复（并发无撕裂；缺口=有界队列满丢语义）");
    CHECK(c.packets > 0, "上行收到包");
    CHECK(st.dec_packets + st.dropped_packets >= (uint32_t)(p1.count + p2.count) - 8u,
          "下行 处理+丢弃 ≥ 提交数-队列容量（满丢最旧，无静默吞包）");

    CHECK(mars_audio_stream_stop() == 0, "stop 返回 0");
    CHECK(mars_audio_playback_stop() == 0, "playback_stop 返回 0");
}

/* ═══════ 3. 只播放、从未录音（P0-3） ═══════ */
static void t_play_only(void)
{
    printf("\n[t3] 只播放、从未录音（P0-3）\n");
    stub_media_reset();

    static uint8_t pkt[100];
    memset(pkt, 0x22, sizeof(pkt));
    CHECK(mars_audio_play_opus(pkt, sizeof(pkt), 16000, 60) == 0, "首次 play 自动初始化下行");
    /* New playback resets downlink stats to zero. The first packet can finish
     * before this thread runs again, so do not snapshot it as a baseline. */
    CHECK(mars_audio_play_opus(pkt, sizeof(pkt), 16000, 60) == 0, "第二次 play");
    CHECK(mars_audio_play_opus(pkt, sizeof(pkt), 16000, 60) == 0, "第三次 play");
    mars_audio_stream_stats_t st;
    for (int i = 0; i < 20; i++) {
        usleep(50000);
        mars_audio_stream_stats(&st);
        if (st.dec_packets >= 3) break;
    }
    printf("  (dec_delta=%u play_samples=%lu out=%d)\n",
           st.dec_packets, stub_media_play_total_samples(),
           stub_media_output_enabled());
    CHECK(st.dec_packets == 3, "本测试新解码 3 包（无崩溃）");
    CHECK(stub_media_play_total_samples() == 960ul * 3, "播放 16k→960 样本/包 × 3");
    CHECK(stub_media_output_enabled() == 1, "输出通路已开启");

    CHECK(mars_audio_playback_stop() == 0, "playback_stop 返回 0");
    CHECK(stub_media_output_enabled() == 0, "只关闭本服务开起的输出通路");
    /* 未 start 过：stop 幂等 */
    CHECK(mars_audio_stream_stop() == 0, "从未 start 的 stop 幂等");
}

/* ═══════ 4. 50 轮启停：无泄漏 + 队列清理 ═══════ */
static void t_50_rounds(void)
{
    printf("\n[t4] 50 轮 start/stop 无泄漏（P0-4）\n");
    stub_media_reset();
    stub_media_set_record_interval_ms(5);
    host_malloc_reset();

    for (int i = 0; i < 50; i++) {
        mars_audio_stream_config_t cfg = MARS_AUDIO_STREAM_CONFIG_DEFAULT();
        cfg.queue_depth = 8;
        int rc = mars_audio_stream_start(&cfg);
        if (rc != 0) {
            printf("  FAIL: 第 %d 轮 start rc=%d\n", i, rc);
            g_fails++;
            break;
        }
        usleep(20000);
        /* 每 10 轮混入下行播放/停止 */
        if (i % 10 == 0) {
            static uint8_t pkt[80];
            memset(pkt, 0x33, sizeof(pkt));
            mars_audio_play_opus(pkt, sizeof(pkt), 24000, 60);
        }
        if (mars_audio_stream_stop() != 0 || mars_audio_playback_stop() != 0) {
            printf("  FAIL: 第 %d 轮 stop\n", i);
            g_fails++;
            break;
        }
    }
    CHECK(host_malloc_count() == host_free_count(),
          "malloc 数 == free 数（无确定性泄漏）");
    printf("  (malloc=%zu free=%zu)\n", host_malloc_count(), host_free_count());

    /* 停止后队列已清：重启 read(timeout=0) 应为 EAGAIN */
    mars_audio_stream_config_t cfg = MARS_AUDIO_STREAM_CONFIG_DEFAULT();
    mars_audio_stream_start(&cfg);
    usleep(50000);
    mars_audio_stream_stop();
    uint8_t buf[64];
    int rc_after = mars_audio_stream_read(buf, sizeof(buf), NULL, 0);
    CHECK(rc_after == -110,
          "重启后 read(timeout=0) == -110（未运行；stop 已清队列且释放资源）");
}

/* ═══════ 5. 分配失败注入与回滚（P0-4/P1-9） ═══════ */
static void t_alloc_fail(void)
{
    printf("\n[t5] 分配失败注入与回滚（P1-9）\n");
    stub_media_reset();

    /* up_init 的 host_malloc 点：q_mem(1) → outbuf(2) → 任务内 frame(3) */
    for (size_t fail_at = 1; fail_at <= 2; fail_at++) {
        host_malloc_reset();
        host_malloc_set_fail_at(fail_at);
        mars_audio_stream_config_t cfg = MARS_AUDIO_STREAM_CONFIG_DEFAULT();
        int rc = mars_audio_stream_start(&cfg);
        printf("  fail_at=%zu rc=%d alloc=%zu leak=%zu\n",
               fail_at, rc, host_malloc_count(), host_malloc_free_count());
        CHECK(rc == -12, "start 返回 ENOMEM");
        CHECK(host_malloc_free_count() == 1,
              "失败路径回滚：唯一未释放的是注入失败的那次（无真实泄漏）");
        host_malloc_set_fail_at(0);
    }
    /* 任务内 frame 分配失败：任务重试不退出，start 成功、stop 正常 */
    host_malloc_reset();
    host_malloc_set_fail_at(3);
    stub_media_set_record_interval_ms(5);
    mars_audio_stream_config_t cfg = MARS_AUDIO_STREAM_CONFIG_DEFAULT();
    int rc = mars_audio_stream_start(&cfg);
    CHECK(rc == 0, "任务内分配失败不影响 start（重试策略）");
    usleep(60000);
    CHECK(mars_audio_stream_stop() == 0, "任务内分配失败后 stop 正常");
    host_malloc_set_fail_at(0);
    CHECK(host_malloc_free_count() == 1, "最终无泄漏（差值=注入失败那次）");
    printf("  (alloc=%zu leak=%zu)\n", host_malloc_count(), host_malloc_free_count());
}

/* ═══════ 6. 错误码/seq/queue_depth/dropped 语义（P1-6） ═══════ */
static void t_errno_seq(void)
{
    printf("\n[t6] 错误码 / 逐包 seq / queue_depth / dropped（P1-6）\n");
    stub_media_reset();

    uint8_t buf[2048];
    CHECK(mars_audio_stream_read(buf, sizeof(buf), NULL, 0) == -110,
          "未 start 的 read == -110（ETIMEDOUT）");

    stub_media_set_record_interval_ms(30);
    mars_audio_stream_config_t cfg = MARS_AUDIO_STREAM_CONFIG_DEFAULT();
    cfg.queue_depth = 2;
    CHECK(mars_audio_stream_start(&cfg) == 0, "start（queue_depth=2）");

    usleep(70000);   /* ≥2 帧（30ms 回调） */
    uint8_t tiny[4];
    mars_audio_packet_info_t info;
    /* 队列有帧时先小缓冲读：-12 且不消费 */
    CHECK(mars_audio_stream_read(tiny, sizeof(tiny), NULL, 0) == -12,
          "小缓冲 read == -12（ENOMEM，区别于空队列）");
    CHECK(mars_audio_stream_read(buf, sizeof(buf), &info, 20) == 0 &&
          info.seq == 1,
          "过小包未被消费：第一帧仍为 seq == 1");
    CHECK(info.payload_len > 0, "payload_len > 0");
    CHECK(mars_audio_stream_read(buf, sizeof(buf), &info, 20) == 0 &&
          info.seq == 2,
          "第二包 seq == 2");

    /* 队列深度 2：灌满后丢最旧 */
    usleep(250000);   /* ~8 帧，depth=2 → dropped ≥ 6 */
    mars_audio_stream_stats_t st;
    mars_audio_stream_stats(&st);
    printf("  enc=%u drop=%u\n", st.enc_packets, st.dropped_packets);
    CHECK(st.dropped_packets >= 6, "queue_depth=2 生效：dropped ≥ 6");

    CHECK(mars_audio_stream_stop() == 0, "stop");

    /* queue_depth=8 对照：无丢 */
    stub_media_set_record_interval_ms(30);
    mars_audio_stream_config_t cfg8 = MARS_AUDIO_STREAM_CONFIG_DEFAULT();
    cfg8.queue_depth = 8;
    mars_audio_stream_start(&cfg8);
    usleep(200000);
    mars_audio_stream_stats(&st);
    CHECK(st.dropped_packets == 0, "queue_depth=8 对照：dropped == 0");
    CHECK(mars_audio_stream_stop() == 0, "stop#2");
}

/* ═══════ 7. 下行满丢（队列满 → 丢最旧，实时性优先） ═══════ */
static void t_down_full(void)
{
    printf("\n[t7] 下行队列满丢最旧（P1-6 双向 dropped）\n");
    stub_media_reset();
    stub_dec_set_delay_ms(200);   /* 解码慢 → 队列积压 */

    static uint8_t pkt[80];
    memset(pkt, 0x44, sizeof(pkt));
    for (int i = 0; i < 20; i++) {
        mars_audio_play_opus(pkt, sizeof(pkt), 16000, 60);
    }
    usleep(2600000);              /* 等 dec 处理完 8 个留包（8×200ms+余量） */
    stub_dec_set_delay_ms(0);

    mars_audio_stream_stats_t st;
    mars_audio_stream_stats(&st);
    printf("  dec=%u drop=%u\n", st.dec_packets, st.dropped_packets);
    CHECK(st.dec_packets + st.dropped_packets == 20,
          "处理+丢弃 == 提交数（无静默吞包）");
    CHECK(st.dropped_packets >= 10, "队列满丢最旧 ≥ 10（8 槽容量）");

    CHECK(mars_audio_playback_stop() == 0, "playback_stop");
}

/* ═══════ 8. 24k → 16k 重采样路径 ═══════ */
static void t_resample_24k(void)
{
    printf("\n[t8] 24kHz → 16kHz 系统层重采样（P1-8）\n");
    stub_media_reset();

    static uint8_t pkt[100];
    memset(pkt, 0x55, sizeof(pkt));
    CHECK(mars_audio_play_opus(pkt, sizeof(pkt), 24000, 60) == 0, "24k 包入队（首次建流水线）");
    /* 等首包解码完、流水线空闲后再取基线，避免把它计入本测试增量 */
    mars_audio_stream_stats_t st0;
    for (int i = 0; i < 20; i++) {
        mars_audio_stream_stats(&st0);
        if (st0.dec_packets >= 1) break;
        usleep(50000);
    }
    unsigned long play0 = stub_media_play_total_samples();
    CHECK(mars_audio_play_opus(pkt, sizeof(pkt), 24000, 60) == 0, "第二个 24k 包入队");
    mars_audio_stream_stats_t st;
    for (int i = 0; i < 20; i++) {
        usleep(50000);
        mars_audio_stream_stats(&st);
        if (st.dec_packets - st0.dec_packets >= 1) break;
    }
    unsigned long play1 = stub_media_play_total_samples();
    printf("  (dec_delta=%u play_samples_delta=%lu)\n",
           st.dec_packets - st0.dec_packets, play1 - play0);
    CHECK(st.dec_packets - st0.dec_packets == 1, "本测试新解码 1 包（累计基线之上）");
    /* 24k 解码 1440 样本 → 重采样 16k 960 样本播放 */
    CHECK(play1 - play0 == 960ul,
          "重采样后播放 960 样本（1440@24k → 960@16k）");
    CHECK(mars_audio_playback_stop() == 0, "playback_stop");
}

/* ═══════ 9. 编码失败注入 ═══════ */
static void t_enc_fail(void)
{
    printf("\n[t9] 编码失败注入（encode_failures 统计）\n");
    stub_media_reset();
    stub_media_set_record_interval_ms(5);

    mars_audio_stream_config_t cfg = MARS_AUDIO_STREAM_CONFIG_DEFAULT();
    CHECK(mars_audio_stream_start(&cfg) == 0, "start");
    stub_enc_set_fail_next(2);
    usleep(200000);
    mars_audio_stream_stats_t st;
    mars_audio_stream_stats(&st);
    printf("  enc=%u enc_fail=%u\n", st.enc_packets, st.encode_failures);
    CHECK(st.encode_failures >= 2, "注入 2 次编码失败已计入 stats");
    CHECK(mars_audio_stream_stop() == 0, "stop");
}

/* ═══════ 10. P0-2：并发首次 play 只建一个解码任务 ═══════ */
typedef struct {
    atomic_int go;
    int rc[16];
    int n;
} once_pusher_t;

static void *once_play_thread(void *arg)
{
    once_pusher_t *o = arg;
    while (!atomic_load_explicit(&o->go, memory_order_acquire)) { usleep(1000); }
    static uint8_t pkt[100];
    memset(pkt, 0x77, sizeof(pkt));
    int idx = __sync_fetch_and_add(&o->n, 1);
    o->rc[idx] = mars_audio_play_opus(pkt, sizeof(pkt), 16000, 60);
    return NULL;
}

static void t_concurrent_first_play(void)
{
    printf("\n[t10] 并发首次 play：解码任务只创建一次（P0-2）\n");
    stub_media_reset();
    stub_task_counters_reset();

    enum { N = 8 };
    pthread_t ths[N];
    once_pusher_t o = {0};
    for (int i = 0; i < N; ++i) {
        pthread_create(&ths[i], NULL, once_play_thread, &o);
    }
    usleep(10000);
    atomic_store_explicit(&o.go, 1, memory_order_release);   /* 放行，制造同时首次进入 */
    for (int i = 0; i < N; ++i) {
        pthread_join(ths[i], NULL);
    }
    int oks = 0;
    for (int i = 0; i < o.n; ++i) {
        if (o.rc[i] == 0) oks++;
    }
    CHECK(oks == N, "全部并发首次 play 返回 0");
    CHECK(stub_dec_task_create_count() == 1,
          "并发首次 play 只创建 1 个解码任务（不是 2 个）");

    /* 等解码消费 */
    usleep(200000);
    CHECK(mars_audio_playback_stop() == 0, "playback_stop");
    CHECK(stub_media_output_enabled() == 0, "stop 后输出恢复为 claim 前(OFF)");
}

/* ═══════ 11. P0-2：并发 start 只建一个编码任务 ═══════ */
static void *once_start_thread(void *arg)
{
    atomic_int *go = arg;
    while (!atomic_load_explicit(go, memory_order_acquire)) { usleep(1000); }
    mars_audio_stream_config_t cfg = MARS_AUDIO_STREAM_CONFIG_DEFAULT();
    int *r = malloc(sizeof(int));
    *r = mars_audio_stream_start(&cfg);
    return r;
}

static void t_concurrent_start(void)
{
    printf("\n[t11] 并发 start：编码任务只创建一次（P0-2）\n");
    stub_media_reset();
    stub_task_counters_reset();
    stub_media_set_record_interval_ms(20);

    enum { N = 6 };
    pthread_t ths[N];
    atomic_int go = 0;
    for (int i = 0; i < N; ++i) {
        pthread_create(&ths[i], NULL, once_start_thread, (void *)&go);
    }
    usleep(10000);
    atomic_store_explicit(&go, 1, memory_order_release);
    int oks = 0;
    for (int i = 0; i < N; ++i) {
        void *r = NULL;
        pthread_join(ths[i], &r);
        if (r != NULL) {
            if (*(int *)r == 0) oks++;
            free(r);
        }
    }
    CHECK(oks >= 1, "至少一个 start 返回 0");
    CHECK(stub_enc_task_create_count() == 1, "并发 start 只创建 1 个编码任务");
    CHECK(mars_audio_stream_stop() == 0, "stop");
    CHECK(mars_audio_playback_stop() == 0, "playback_stop");
}

/* ═══════ 12. P0-2：play/playback_stop、start/stop 交错压力（无崩溃/无双重任务） ═══════ */
static void *play_loop_thread(void *arg)
{
    atomic_int *stop = arg;
    static uint8_t pkt[64];
    memset(pkt, 0x88, sizeof(pkt));
    while (!atomic_load_explicit(stop, memory_order_acquire)) {
        mars_audio_play_opus(pkt, sizeof(pkt), 16000, 60);
        usleep(2000);
    }
    return NULL;
}

static void t_lifecycle_interleave(void)
{
    printf("\n[t12] play/playback_stop 与 start/stop 交错压力（P0-2）\n");
    stub_media_reset();
    stub_task_counters_reset();
    stub_media_set_record_interval_ms(10);

    for (int round = 0; round < 5; ++round) {
        atomic_int stop_flag = 0;
        pthread_t pt;
        pthread_create(&pt, NULL, play_loop_thread, (void *)&stop_flag);
        for (int i = 0; i < 3; ++i) {
            mars_audio_stream_config_t cfg = MARS_AUDIO_STREAM_CONFIG_DEFAULT();
            mars_audio_stream_start(&cfg);
            usleep(5000);
            mars_audio_stream_stop();
            mars_audio_playback_stop();
            usleep(2000);
        }
        atomic_store_explicit(&stop_flag, 1, memory_order_release);
        pthread_join(pt, NULL);
        mars_audio_playback_stop();
    }
    CHECK(stub_enc_task_create_count() <= 200, "编码任务创建次数有界（无泄漏/无无限双启）");
    CHECK(stub_dec_task_create_count() <= 200, "解码任务创建次数有界（交错压力下每个 play-after-stop 重建一个）");
    CHECK(mars_audio_stream_stop() == 0, "最终 stop 幂等");
    CHECK(mars_audio_playback_stop() == 0, "最终 playback_stop 幂等");
    CHECK(stub_tasks_running() == 0, "交错后无残留任务");
}

/* ═══════ 13. P0-3：semaphore/mutex/task 创建失败注入与回滚 ═══════ */
static void t_sync_fault_injection(void)
{
    printf("\n[t13] sem/mutex/task 创建失败注入与回滚（P0-3）\n");
    stub_media_reset();
    stub_media_set_record_interval_ms(20);
    /* 生命周期 mutex 本身创建失败必须可观测，不能静默进入半初始化。 */
    mars_audio_stream_reset_lifecycle_for_test();
    stub_sem_fail_next(1);
    mars_audio_stream_init();
    mars_audio_stream_config_t cfg = MARS_AUDIO_STREAM_CONFIG_DEFAULT();
    CHECK(mars_audio_stream_start(&cfg) == -12, "lifecycle mutex 创建失败时 start 返回 -12");
    stub_sem_fail_next(0);
    mars_audio_stream_init();

    /* 回到刚上电状态：同步原语尚未创建，否则 up_sync_create 因复用而跳过创建 */
    mars_audio_stream_reset_for_test();

    int rc = 0;
    /* 上行共 4 个同步对象：逐个失败都必须整体回滚。 */
    for (int fail_at = 1; fail_at <= 4; ++fail_at) {
        mars_audio_stream_reset_for_test();
        stub_sem_fail_next(fail_at);
        rc = mars_audio_stream_start(&cfg);
        CHECK(rc == -12, "上行每个 sem/mutex 创建点失败均返回 -12");
        CHECK(stub_tasks_running() == 0, "上行同步对象失败后无残留任务");
        CHECK(mars_audio_stream_is_running() == 0, "上行同步对象失败后未运行");
        stub_sem_fail_next(0);
    }

    /* 清除注入后再次 start 必须成功（句柄已复位可重建） */
    mars_audio_stream_reset_for_test();
    rc = mars_audio_stream_start(&cfg);
    CHECK(rc == 0, "sem 失败回滚后再次 start 成功");
    CHECK(mars_audio_stream_stop() == 0, "再次 stop 正常");

    /* 任务创建失败注入：up_init 成功但 xTaskCreate 失败 → 回滚录音与内存 */
    stub_task_fail_next(1);
    rc = mars_audio_stream_start(&cfg);
    CHECK(rc == -12, "task 创建失败时 start 返回 -12");
    CHECK(stub_tasks_running() == 0, "task 失败后无残留任务");
    stub_task_fail_next(0);
    rc = mars_audio_stream_start(&cfg);
    CHECK(rc == 0, "task 失败回滚后再次 start 成功");
    CHECK(mars_audio_stream_stop() == 0, "再次 stop 正常");

    /* 下行共 3 个同步对象：逐个失败都必须整体回滚。 */
    static uint8_t pkt[64];
    memset(pkt, 0x99, sizeof(pkt));
    for (int fail_at = 1; fail_at <= 3; ++fail_at) {
        mars_audio_stream_reset_for_test();
        stub_sem_fail_next(fail_at);
        rc = mars_audio_play_opus(pkt, sizeof(pkt), 16000, 60);
        CHECK(rc == -12, "下行每个 sem/mutex 创建点失败均返回 -12");
        CHECK(stub_tasks_running() == 0, "下行同步对象失败后无残留任务");
        CHECK(stub_media_output_enabled() == 0, "下行同步对象失败后无输出残留");
        stub_sem_fail_next(0);
    }
    mars_audio_stream_reset_for_test();
    rc = mars_audio_play_opus(pkt, sizeof(pkt), 16000, 60);
    CHECK(rc == 0, "下行 sem 失败回滚后 play 成功");
    CHECK(mars_audio_playback_stop() == 0, "playback_stop");
}

/* ═══════ 14. P0-5：输出通路所有权 claim/release ═══════ */
static void t_output_ownership(void)
{
    printf("\n[t14] 输出通路所有权：原状态恢复 / 首次写失败 / 其他所有者（P0-5）\n");

    /* 情形 A：输出原本已被"别人"打开 → 本服务播放后停止，不得误关别人的输出 */
    stub_media_reset();
    stub_media_set_output_enabled(1);   /* 模拟其他功能已开启输出 */
    static uint8_t pkt[80];
    memset(pkt, 0xAA, sizeof(pkt));
    CHECK(mars_audio_play_opus(pkt, sizeof(pkt), 16000, 60) == 0, "A: play 入队");
    usleep(150000);
    CHECK(mars_audio_playback_stop() == 0, "A: playback_stop");
    CHECK(stub_media_output_enabled() == 1,
          "A: 输出原本已开，停止后保持开启（不误关别人）");

    /* 情形 B：首次写失败（通路已开但 write 失败）→ release 仍恢复 claim 前(OFF) */
    stub_media_reset();
    stub_media_set_play_fail_next(2);   /* 前两次写失败 */
    CHECK(mars_audio_play_opus(pkt, sizeof(pkt), 16000, 60) == 0, "B: play 入队");
    usleep(150000);
    CHECK(mars_audio_playback_stop() == 0, "B: playback_stop");
    CHECK(stub_media_output_enabled() == 0,
          "B: claim 前是 OFF，即使首次写失败也不留输出开启");

    /* 情形 C：其他所有者占用输出 → play 首次 init 返回明确错误，不建解码任务 */
    stub_media_reset();
    stub_media_foreign_claim(1);
    int dec_before = stub_dec_task_create_count();
    int rc = mars_audio_play_opus(pkt, sizeof(pkt), 16000, 60);
    CHECK(rc == -16, "C: 输出被占用时 play 返回 -16(EBUSY)");
    CHECK(stub_dec_task_create_count() == dec_before, "C: 冲突时不创建解码任务");
    stub_media_foreign_claim(0);
    CHECK(mars_audio_playback_stop() == 0, "C: playback_stop 幂等");
}

/* ═══════ 15. P1-4：frame_ms 白名单 ═══════ */
static void t_frame_ms_whitelist(void)
{
    printf("\n[t15] frame_ms 白名单校验（P1-4）\n");
    stub_media_reset();
    static uint8_t pkt[64];
    memset(pkt, 0xBB, sizeof(pkt));
    CHECK(mars_audio_play_opus(pkt, sizeof(pkt), 16000, 30) == -22,
          "frame_ms=30 非白名单 → -22 EINVAL");
    CHECK(mars_audio_play_opus(pkt, sizeof(pkt), 16000, 77) == -22,
          "frame_ms=77 非白名单 → -22 EINVAL");
    CHECK(mars_audio_play_opus(pkt, sizeof(pkt), 16000, 20) == 0, "frame_ms=20 合法");
    CHECK(mars_audio_playback_stop() == 0, "playback_stop");
}

/* ═══════ 16. P0-3：首包失败不留任务/claim ═══════ */
static void t_first_packet_rollback(void)
{
    printf("\n[t16] 首包超长 / task 创建失败：零残留（P0-3）\n");
    stub_media_reset();
    mars_audio_stream_reset_for_test();

    /* 1) 超长首包：所有校验在 init/claim/task 之前，绝不能建任务 */
    static uint8_t huge[2049];
    memset(huge, 0x77, sizeof(huge));
    int dec_before = stub_dec_task_create_count();
    int tasks_before = stub_tasks_running();
    int rc = mars_audio_play_opus(huge, sizeof(huge), 16000, 60);
    CHECK(rc == -22, "首包 2049B → -22 EINVAL（在 init 之前拦截）");
    CHECK(stub_dec_task_create_count() == dec_before, "超长首包不创建解码任务");
    CHECK(stub_tasks_running() == tasks_before, "超长首包无任务残留");
    CHECK(stub_media_output_enabled() == 0, "超长首包不开启输出通路");

    /* 2) 首包临时 malloc 失败：发生在 init/claim/task 之前。 */
    host_malloc_reset();
    host_malloc_set_fail_at(1);
    static uint8_t pkt[64];
    memset(pkt, 0x78, sizeof(pkt));
    int dec_malloc_before = stub_dec_task_create_count();
    rc = mars_audio_play_opus(pkt, sizeof(pkt), 16000, 60);
    CHECK(rc == -12, "首包临时 malloc 失败 → -12");
    CHECK(stub_dec_task_create_count() == dec_malloc_before, "临时 malloc 失败不创建任务");
    CHECK(stub_media_output_enabled() == 0, "临时 malloc 失败不 claim/开启输出");
    host_malloc_set_fail_at(0);

    /* 3) task 创建失败：down_init/claim 成功但 xTaskCreate 失败 → 全回滚 */
    stub_task_fail_next(1);
    int tasks2 = stub_tasks_running();
    rc = mars_audio_play_opus(pkt, sizeof(pkt), 16000, 60);
    CHECK(rc == -12, "首包 task 创建失败 → -12");
    CHECK(stub_tasks_running() == tasks2, "task 失败后无残留任务");
    CHECK(stub_media_output_enabled() == 0, "task 失败后输出通路已 release");
    stub_task_fail_next(0);

    /* 4) 再次正常 play 成功 */
    CHECK(mars_audio_play_opus(pkt, sizeof(pkt), 16000, 60) == 0, "回滚后正常 play 成功");
    usleep(120000);
    CHECK(mars_audio_playback_stop() == 0, "playback_stop");
}

/* ═══════ 17. P1-1：先下行后上行，下行统计保留 ═══════ */
static void t_stats_isolation(void)
{
    printf("\n[t17] 先下行解码，再启动上行，下行统计保留（P1-1）\n");
    stub_media_reset();
    static uint8_t pkt[64];
    memset(pkt, 0x81, sizeof(pkt));
    CHECK(mars_audio_play_opus(pkt, sizeof(pkt), 16000, 60) == 0, "下行先启动");
    usleep(150000);
    mars_audio_stream_stats_t st0;
    mars_audio_stream_stats(&st0);
    CHECK(st0.dec_packets > 0, "下行已解码至少 1 包");

    /* 上行启动：绝不能清零下行统计 */
    stub_media_set_record_interval_ms(20);
    mars_audio_stream_config_t cfg = MARS_AUDIO_STREAM_CONFIG_DEFAULT();
    CHECK(mars_audio_stream_start(&cfg) == 0, "上行在下行运行时启动");
    usleep(100000);
    mars_audio_stream_stats_t st;
    mars_audio_stream_stats(&st);
    CHECK(st.dec_packets >= st0.dec_packets, "启动上行不清空下行 dec_packets");
    CHECK(st.play_failures == st0.play_failures, "启动上行不清空下行 play_failures");
    CHECK(mars_audio_stream_stop() == 0, "stop 上行");
    CHECK(mars_audio_playback_stop() == 0, "playback_stop");
}

/* ═══════ 18. P0-4：直接并发 claim 与 release/claim 交错 ═══════ */
struct claimant_arg { atomic_int *go; int rc; };
static void *claimant_run(void *p)
{
    struct claimant_arg *a = p;
    while (!atomic_load_explicit(a->go, memory_order_acquire)) usleep(100);
    a->rc = mars_media_output_claim();
    return NULL;
}
static void *release_run(void *p)
{
    atomic_int *go = p;
    while (!atomic_load_explicit(go, memory_order_acquire)) usleep(100);
    mars_media_output_release();
    return NULL;
}
static void t_concurrent_claimants(void)
{
    printf("\n[t18] production 等价的并发 claim/release 状态机（P0-4）\n");
    stub_media_reset();
    atomic_int go = 0;
    struct claimant_arg a1 = { &go, -99 };
    struct claimant_arg a2 = { &go, -99 };
    pthread_t t1, t2;
    pthread_create(&t1, NULL, claimant_run, &a1);
    pthread_create(&t2, NULL, claimant_run, &a2);
    atomic_store_explicit(&go, 1, memory_order_release);
    pthread_join(t1, NULL);
    pthread_join(t2, NULL);
    CHECK((a1.rc == ESP_OK) + (a2.rc == ESP_OK) == 1,
          "两个直接并发 claimant 成功数恰好为 1");
    mars_media_output_release();
    CHECK(stub_media_output_enabled() == 0, "释放后恢复 claim 前 OFF");

    /* 旧 owner release 与新 claim 同时发生：新 claim 成功或忙都可，但清理后必须 OFF。 */
    int interleave_ok = 1;
    int16_t one_sample = 0;
    for (int i = 0; i < 200; ++i) {
        if (mars_media_output_claim() != ESP_OK) {
            interleave_ok = 0;
            break;
        }
        if (mars_media_play(&one_sample, 1, 0) != ESP_OK ||
            stub_media_output_enabled() != 1) {
            interleave_ok = 0;
            break;
        }
        atomic_store_explicit(&go, 0, memory_order_relaxed);
        struct claimant_arg next = { &go, -99 };
        pthread_t rel, cla;
        pthread_create(&rel, NULL, release_run, &go);
        pthread_create(&cla, NULL, claimant_run, &next);
        atomic_store_explicit(&go, 1, memory_order_release);
        pthread_join(rel, NULL);
        pthread_join(cla, NULL);
        if (next.rc == ESP_OK) mars_media_output_release();
        if (stub_media_output_enabled() != 0) {
            interleave_ok = 0;
            break;
        }
    }
    mars_media_output_release(); /* 失败分支也清理，避免污染后续用例 */
    CHECK(interleave_ok, "200 轮 release/claim 交错清理后均恢复 OFF");
}

/* ═══════ 19. P0-2：read/stop 与 start/stop 真并发 ═══════ */
typedef struct {
    atomic_int *go;
    int rounds;
} lifecycle_worker_arg_t;

static void *start_loop_thread(void *p)
{
    lifecycle_worker_arg_t *a = p;
    while (!atomic_load_explicit(a->go, memory_order_acquire)) usleep(100);
    for (int i = 0; i < a->rounds; ++i) {
        mars_audio_stream_config_t cfg = MARS_AUDIO_STREAM_CONFIG_DEFAULT();
        mars_audio_stream_start(&cfg);
        usleep(500);
    }
    return NULL;
}

static void *stop_loop_thread(void *p)
{
    lifecycle_worker_arg_t *a = p;
    while (!atomic_load_explicit(a->go, memory_order_acquire)) usleep(100);
    for (int i = 0; i < a->rounds; ++i) {
        mars_audio_stream_stop();
        usleep(500);
    }
    return NULL;
}

static void *single_stop_thread(void *p)
{
    int *rc = p;
    *rc = mars_audio_stream_stop();
    return NULL;
}

static void t_read_stop_and_start_stop(void)
{
    printf("\n[t19] read/stop 与 start/stop 真并发（P0-2）\n");
    stub_media_reset();
    stub_media_set_record_interval_ms(2);
    mars_audio_stream_config_t cfg = MARS_AUDIO_STREAM_CONFIG_DEFAULT();
    CHECK(mars_audio_stream_start(&cfg) == 0, "read/stop 对抗前 start");

    collector_t c = {0};
    c.timeout_ms = 20;
    pthread_t reader, stopper;
    int stop_rc = -999;
    pthread_create(&reader, NULL, collector_thread, &c);
    usleep(20000);
    pthread_create(&stopper, NULL, single_stop_thread, &stop_rc);
    pthread_join(stopper, NULL);
    atomic_store_explicit(&c.stop, 1, memory_order_release);
    pthread_join(reader, NULL);
    CHECK(stop_rc == 0, "read 并发期间 stop 成功");
    CHECK(stub_tasks_running() == 0, "read/stop 后无残留任务");

    atomic_int go = 0;
    lifecycle_worker_arg_t arg = { &go, 80 };
    pthread_t starter, stop_loop;
    pthread_create(&starter, NULL, start_loop_thread, &arg);
    pthread_create(&stop_loop, NULL, stop_loop_thread, &arg);
    atomic_store_explicit(&go, 1, memory_order_release);
    pthread_join(starter, NULL);
    pthread_join(stop_loop, NULL);
    CHECK(mars_audio_stream_stop() == 0, "真并发 start/stop 后最终 stop");
    CHECK(stub_tasks_running() == 0, "真并发 start/stop 后无残留任务");
}

/* ═══════ 20. P0-2：stop 超时后的退出 token 不得污染下一轮 ═══════ */
static void t_stop_timeout_recovery(void)
{
    printf("\n[t20] playback_stop 超时后的重试与下一轮（P0-2）\n");
    stub_media_reset();
    stub_dec_set_delay_ms(1300);
    static uint8_t pkt[64];
    memset(pkt, 0x91, sizeof(pkt));
    CHECK(mars_audio_play_opus(pkt, sizeof(pkt), 16000, 60) == 0, "慢解码首包入队");
    usleep(50000); /* 确保解码任务进入 1.3 s 延迟 */
    CHECK(mars_audio_playback_stop() == -110, "首次 stop 在 1 s 后超时");
    CHECK(mars_audio_play_opus(pkt, sizeof(pkt), 16000, 60) == -16,
          "超时后的 STOPPING 状态拒绝新包");
    usleep(400000); /* 任务完成延迟、观察 stop 请求并给出退出 token */
    CHECK(mars_audio_playback_stop() == 0, "第二次 stop 消费真实退出 token 并收尾");

    stub_dec_set_delay_ms(0);
    CHECK(mars_audio_play_opus(pkt, sizeof(pkt), 16000, 60) == 0, "超时恢复后下一轮可启动");
    usleep(100000);
    CHECK(mars_audio_playback_stop() == 0, "下一轮 stop 不会误吃旧 token");
    CHECK(stub_tasks_running() == 0, "超时恢复后无残留任务");
}

static void t_finish_drain_tail(void)
{
    printf("\n[finish] retain final packets and pad partial frame\n");
    stub_media_reset(); stub_media_set_record_interval_ms(80);
    CHECK(mars_audio_stream_start(NULL) == 0, "finish start");
    usleep(110000);
    CHECK(mars_audio_stream_finish() == 0, "finish joins capture and encoder");
    unsigned long expected = (stub_media_record_calls() * 1280ul + 959) / 960;
    CHECK(!mars_audio_stream_is_running(), "capture no longer running");
    CHECK(mars_audio_stream_finish() == 0, "finish idempotent");
    CHECK(mars_audio_stream_start(NULL) == -16, "retained queue blocks restart");
    uint8_t data[2048]; mars_audio_packet_info_t info; unsigned long count = 0;
    while(mars_audio_stream_read(data,sizeof(data),&info,0) == 0) ++count;
    CHECK(count == expected, "all final frames remain readable including padded tail");
    mars_audio_stream_stats_t stats; mars_audio_stream_stats(&stats);
    CHECK(count == stats.enc_packets && stats.dropped_packets == 0, "no final packet loss");
    CHECK(mars_audio_stream_stop() == 0, "stop frees retained queue");
    CHECK(mars_audio_stream_read(data,sizeof(data),&info,0) == -110, "read after release rejected");
    CHECK(mars_audio_stream_start(NULL) == 0, "restart after drain");
    CHECK(mars_audio_stream_stop() == 0, "final stop");
}

static void t_pcm_levels(void)
{
    uint16_t up=1,down=1;
    mars_audio_levels(&up,&down);
    CHECK(up==0 && down==0,"音量初始为零");
    CHECK(mars_audio_stream_start(NULL)==0,"音量测试启动录音");
    usleep(180000);
    mars_audio_levels(&up,&down);
    CHECK(up>0 && up<=1000 && down==0,"采集 PCM 产生音量，播放方向保持零");
    CHECK(mars_audio_stream_stop()==0,"音量测试停止录音");
    mars_audio_levels(&up,&down);CHECK(up==0,"停止后清除录音音量");
    uint8_t opus[3]={1,2,3};
    CHECK(mars_audio_play_opus(opus,sizeof(opus),24000,60)==0,"音量测试提交下行");
    usleep(100000);
    mars_audio_levels(&up,&down);
    CHECK(up==0 && down==1000,"解码 PCM 产生封顶音量，两方向隔离");
    vTaskDelay(pdMS_TO_TICKS(300));mars_audio_levels(NULL,&down);
    CHECK(down==0,"超过250ms没有新PCM时音量归零");
    CHECK(mars_audio_playback_stop()==0,"音量测试停止播放");
    mars_audio_levels(NULL,NULL);
}

static void t_packet_effects(void)
{
    stub_media_reset();
    uint8_t pkt[12]={0};
    CHECK(mars_audio_play_opus_effect(pkt,sizeof pkt,16000,60,255)==-22,"非法效果在分配前拒绝");
    CHECK(mars_audio_play_opus_effect(pkt,sizeof pkt,16000,60,MARS_VOICE_ROBOT)==0,"机器人包入队");
    CHECK(mars_audio_play_opus_effect(pkt,sizeof pkt,16000,60,MARS_VOICE_WARM)==0,"低沉包入队");
    CHECK(mars_audio_play_opus(pkt,sizeof pkt,16000,60)==0,"旧播放API原声包入队");
    for(int i=0;i<200 && stub_media_play_total_samples()<2880;++i)usleep(1000);
    CHECK(stub_media_play_total_samples()==2880,"变声不改变播放样本数");
    CHECK(stub_media_first_sample(0)==0x5555/5,"逐包机器人效果正确");
    CHECK(stub_media_first_sample(1)==(0x5555+3*(0x5555/4))/4,"逐包低沉效果正确");
    CHECK(stub_media_first_sample(2)==0x5555,"旧API不会继承前包效果");
    CHECK(mars_audio_playback_stop()==0,"变声播放停止屏障完成");
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    printf("═══ MARS P1 audio stream service host tests ═══\n");
    /* P0-2：创建生命周期锁（与固件 app_main 中一致），否则 start/play 返回 -12。 */
    mars_audio_stream_init();
    t_packet_effects();
    t_pcm_levels();
    t_real_consume();
    t_concurrent();
    t_play_only();
    t_50_rounds();
    t_alloc_fail();
    t_errno_seq();
    t_down_full();
    t_resample_24k();
    t_enc_fail();
    /* 第三轮新增：P0-2/P0-3/P0-5/P1-4 */
    t_concurrent_first_play();
    t_concurrent_start();
    t_lifecycle_interleave();
    t_sync_fault_injection();
    t_output_ownership();
    t_frame_ms_whitelist();
    /* 第四轮新增：P0-3 首包回滚 / P1-1 统计隔离 / P0-4 双 claimant */
    t_first_packet_rollback();
    t_stats_isolation();
    t_concurrent_claimants();
    t_read_stop_and_start_stop();
    t_stop_timeout_recovery();
    t_finish_drain_tail();

    /* P0-2(7)：全程同时存活任务峰值每方向 ≤1 */
    CHECK(stub_enc_peak() <= 1, "全程同时存活编码任务峰值 ≤1");
    CHECK(stub_dec_peak() <= 1, "全程同时存活解码任务峰值 ≤1");

    printf("\n═══ RESULT: %s（%d 个断言失败）═══\n", g_fails == 0 ? "PASS" : "FAIL", g_fails);
    return g_fails == 0 ? 0 : 1;
}
