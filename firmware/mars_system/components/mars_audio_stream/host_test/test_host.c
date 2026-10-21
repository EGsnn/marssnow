/* host_test：mars_audio_stream 纯 C 核心离线单测（x86/macOS 直接编译运行）
 *
 * 覆盖：
 *  1. 拼帧器：80ms(1280)@16k → 60ms(960) 分帧（3×1280 → 4×960）、不足帧、
 *     超限丢弃、数据保真
 *  2. 有界包队列：FIFO 顺序、满丢最旧 + 计数、空 pop、缓冲过小、
 *     参数错误（init/push 非法输入）、包长恢复
 *  3. 重采样：24k→16k 比例与内容、跨缓冲连续（相位/插值）、48k→16k、
 *     参数错误返回 0
 *  4. 复用循环：重复 init/push/pull（模拟 start/stop/close 反复的生命周期）
 */

#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mars_audio_stream_core.h"
#include "mars_resample.h"

static int g_pass = 0;
static int g_fail = 0;

#define CHECK(cond, msg)                                       \
    do {                                                       \
        if (cond) {                                            \
            ++g_pass;                                          \
        } else {                                               \
            ++g_fail;                                          \
            printf("  FAIL: %s (line %d)\n", msg, __LINE__);   \
        }                                                      \
    } while (0)

/* ═══════════ 1. 拼帧器 ═══════════ */

static void test_frame_acc(void)
{
    printf("[拼帧器] 80ms(1280)@16k → 60ms(960)\n");
    mars_frame_acc_t acc;
    mars_frame_acc_init(&acc, 960);

    /* 3×1280 = 3840 = 4×960 */
    int16_t in[1280];
    for (int i = 0; i < 1280; ++i) {
        in[i] = (int16_t)(i & 0x7FFF);
    }
    int16_t out[960];
    size_t dropped = 0;
    int frames = 0;

    mars_frame_acc_push(&acc, in, 1280, &dropped);
    CHECK(mars_frame_acc_available(&acc) == 1280, "推入 1280 后累积 1280");
    CHECK(mars_frame_acc_has_frame(&acc), "1280 >= 960 应有帧");
    CHECK(mars_frame_acc_pull(&acc, out) == 1, "第一次拉出一帧");
    CHECK(mars_frame_acc_available(&acc) == 320, "剩余 320");
    ++frames;
    CHECK(mars_frame_acc_pull(&acc, out) == 0, "320 < 960 拉不出");
    mars_frame_acc_push(&acc, in, 1280, &dropped);
    CHECK(mars_frame_acc_available(&acc) == 1600, "320+1280=1600");
    CHECK(mars_frame_acc_pull(&acc, out) == 1, "第二帧");
    ++frames;
    CHECK(mars_frame_acc_available(&acc) == 640, "剩余 640");
    mars_frame_acc_push(&acc, in, 1280, &dropped);
    CHECK(mars_frame_acc_available(&acc) == 1920, "640+1280=1920");
    CHECK(mars_frame_acc_pull(&acc, out) == 1, "第三帧");
    ++frames;
    CHECK(mars_frame_acc_pull(&acc, out) == 1, "第四帧（1920-960=960）");
    ++frames;
    CHECK(frames == 4, "3×1280 恰好 4×960");
    CHECK(mars_frame_acc_available(&acc) == 0, "3840 消耗完毕");
    CHECK(dropped == 0, "无超限丢弃");

    /* 数据保真：第一帧应等于 in[0..959] */
    mars_frame_acc_init(&acc, 960);
    mars_frame_acc_push(&acc, in, 1280, NULL);
    mars_frame_acc_pull(&acc, out);
    int same = 1;
    for (int i = 0; i < 960; ++i) {
        if (out[i] != in[i]) {
            same = 0;
            break;
        }
    }
    CHECK(same, "拼帧数据保真（首帧=输入前 960 样本）");

    /* 超限丢弃：push 超过 MAX 时丢弃最旧部分 */
    mars_frame_acc_init(&acc, 960);
    int16_t big[MARS_FRAME_ACC_MAX_SAMPLES];
    memset(big, 0, sizeof(big));
    dropped = 0;
    mars_frame_acc_push(&acc, big, MARS_FRAME_ACC_MAX_SAMPLES, &dropped);
    CHECK(acc.n == MARS_FRAME_ACC_MAX_SAMPLES, "MAX 推入不超限");
    int16_t one = 0x1234;
    mars_frame_acc_push(&acc, &one, 1, &dropped);
    CHECK(acc.n == MARS_FRAME_ACC_MAX_SAMPLES, "超限后仍封顶 MAX");
    CHECK(dropped == 1, "超限丢弃计数 = 1");
    CHECK(acc.buf[acc.n - 1] == 0x1234, "最新样本保留在尾部");

    /* 帧长参数校验 */
    mars_frame_acc_init(&acc, 0);
    CHECK(acc.frame_samples == MARS_FRAME_ACC_MAX_SAMPLES, "frame_samples=0 回退到 MAX");
    printf("  通过 %d 断言\n", g_pass - (g_pass - 0));
}

/* ═══════════ 2. 有界包队列 ═══════════ */

static void test_pkt_queue(void)
{
    printf("[有界包队列] 4B 头(len2+seq2)\n");
    /* 槽布局：前 MARS_PKT_HDR_SIZE(4) 字节 = len(2B LE) + seq(2B LE)，其后为负载。
     * 槽长 64 → 最大负载 = 64 - 4 = 60。 */
    static uint8_t mem[8 * 64];
    mars_pkt_queue_t q;

    /* 参数错误 */
    CHECK(mars_pkt_queue_init(NULL, mem, 64, 8) == -1, "init null 队列");
    CHECK(mars_pkt_queue_init(&q, NULL, 64, 8) == -1, "init null 内存");
    CHECK(mars_pkt_queue_init(&q, mem, MARS_PKT_HDR_SIZE, 8) == -1, "init 槽过小(=hdr)");
    CHECK(mars_pkt_queue_init(&q, mem, 64, 0) == -1, "init 深度 0");
    CHECK(mars_pkt_queue_init(&q, mem, 64, 33) == -1, "init 深度超限");

    CHECK(mars_pkt_queue_init(&q, mem, 64, 4) == 0, "init 正常");
    CHECK(mars_pkt_queue_count(&q) == 0, "初始空");

    /* 非法 push */
    CHECK(mars_pkt_queue_push(&q, NULL, 4, 1) == -1, "push null 包");
    CHECK(mars_pkt_queue_push(&q, "abc", 0, 1) == -1, "push 空长度");
    CHECK(mars_pkt_queue_push(&q, "x", 61, 1) == -1, "push 超槽（64-4=60 负载上限，61 越界）");
    char big60[60];
    memset(big60, 'B', sizeof(big60));
    CHECK(mars_pkt_queue_push(&q, big60, 60, 1) == 0, "push 60B 恰好填满槽位合法");

    /* FIFO 顺序 + 长度恢复 + seq 原样出队 */
    const char *pkts[] = { "pkt-a", "pkt-bb", "pkt-ccc" };
    for (int i = 0; i < 3; ++i) {
        CHECK(mars_pkt_queue_push(&q, pkts[i], strlen(pkts[i]), (uint16_t)(100 + i)) == 0,
              "push 顺序包");
    }
    CHECK(mars_pkt_queue_count(&q) == 4, "4 包在队");
    char dst[64];
    size_t len = 0;
    uint16_t seq = 0;
    CHECK(mars_pkt_queue_pop(&q, dst, sizeof(dst), &len, &seq) == 0, "pop 首包");
    CHECK(len == 60 && seq == 1, "首包长恢复(60B) 且 seq==1");
    CHECK(memcmp(dst, big60, 60) == 0, "首包内容 = big60");
    CHECK(mars_pkt_queue_pop(&q, dst, sizeof(dst), &len, &seq) == 0, "pop pkt-a");
    CHECK(len == 5 && seq == 100 && memcmp(dst, "pkt-a", 5) == 0, "pkt-a 内容+seq=100");
    CHECK(mars_pkt_queue_pop(&q, dst, sizeof(dst), &len, &seq) == 0 && seq == 101, "pkt-bb seq=101");
    CHECK(mars_pkt_queue_pop(&q, dst, sizeof(dst), &len, &seq) == 0 && seq == 102, "pkt-ccc seq=102");
    CHECK(mars_pkt_queue_count(&q) == 0, "队列空");
    CHECK(mars_pkt_queue_pop(&q, dst, sizeof(dst), &len, &seq) == -1, "空队列 pop -1");

    /* seq 16 位环绕：写入 0xFFFE, 0xFFFF, 0x0000, 0x0001，应原样出队 */
    mars_pkt_queue_init(&q, mem, 64, 4);
    uint16_t wrap_in[4] = { 0xFFFEu, 0xFFFFu, 0x0000u, 0x0001u };
    for (int i = 0; i < 4; ++i) {
        char t[4] = "w";
        CHECK(mars_pkt_queue_push(&q, t, 1, wrap_in[i]) == 0, "push 环绕 seq");
    }
    int wrap_ok = 1;
    for (int i = 0; i < 4; ++i) {
        if (mars_pkt_queue_pop(&q, dst, sizeof(dst), &len, &seq) != 0 || seq != wrap_in[i]) {
            wrap_ok = 0;
        }
    }
    CHECK(wrap_ok, "16 位 seq 环绕(0xFFFE→0x0001)原样出队");

    /* 缓冲过小返回 -2 且不消费 */
    mars_pkt_queue_init(&q, mem, 64, 4);
    mars_pkt_queue_push(&q, "0123456789", 10, 7);
    CHECK(mars_pkt_queue_pop(&q, dst, 5, &len, &seq) == -2, "pop 缓冲过小 -2");
    CHECK(mars_pkt_queue_count(&q) == 1, "缓冲过小不消费包");
    CHECK(mars_pkt_queue_pop(&q, dst, sizeof(dst), &len, &seq) == 0 && seq == 7 && len == 10,
          "重试 pop 成功，seq=7 包仍在队首");

    /* out_seq == NULL 时不应崩溃 */
    mars_pkt_queue_init(&q, mem, 64, 4);
    mars_pkt_queue_push(&q, "abc", 3, 9);
    CHECK(mars_pkt_queue_pop(&q, dst, sizeof(dst), &len, NULL) == 0, "out_seq=NULL pop 正常");

    /* 满 → 丢最旧 + 计数 */
    mars_pkt_queue_init(&q, mem, 64, 4);
    for (int i = 0; i < 4; ++i) {
        char tmp[8];
        snprintf(tmp, sizeof(tmp), "p%d", i);
        CHECK(mars_pkt_queue_push(&q, tmp, 3, (uint16_t)(20 + i)) == 0, "填满队列");
    }
    CHECK(mars_pkt_queue_full(&q), "队列满");
    CHECK(mars_pkt_queue_push(&q, "p4", 3, 24) == 0, "满时 push 新包成功");
    CHECK(mars_pkt_queue_dropped(&q) == 1, "丢包计数 = 1");
    CHECK(mars_pkt_queue_count(&q) == 4, "仍是 4 包");
    CHECK(mars_pkt_queue_pop(&q, dst, sizeof(dst), &len, &seq) == 0 && len == 3 &&
          seq == 21 && memcmp(dst, "p1", 3) == 0, "最旧 p0(seq20) 被丢，队首是 p1(seq21)");

    /* reset：清空包但保留 dropped/overflows 累计 */
    mars_pkt_queue_reset(&q);
    CHECK(mars_pkt_queue_count(&q) == 0, "reset 后队列空");
    CHECK(mars_pkt_queue_dropped(&q) == 1, "reset 后 dropped 累计保留");
    printf("  通过\n");
}

/* ═══════════ 3. 重采样 ═══════════ */

static void test_resample(void)
{
    printf("[重采样] 24k→16k / 48k→16k / 跨缓冲连续\n");

    /* 24k→16k：24000 输入 → 16000 输出（3:2） */
    const uint32_t in_rate = 24000, out_rate = 16000;
    const size_t in_n = 24000;
    int16_t *in = malloc(in_n * sizeof(int16_t));
    int16_t *out = malloc((in_n * out_rate / in_rate + 8) * sizeof(int16_t));
    for (size_t i = 0; i < in_n; ++i) {
        in[i] = (int16_t)(12000 * sin(2 * 3.14159265 * 440.0 * i / in_rate));
    }
    mars_resample_t r;
    mars_resample_init(&r, in_rate, out_rate);
    size_t out_n = mars_resample_process(&r, in, in_n, out, in_n * out_rate / in_rate + 8);
    CHECK(out_n == 16000, "24k→16k 单次输出 16000 样本");

    /* 内容校验：能量量级正确（>0.5×原均方） */
    double e = 0;
    for (size_t i = 0; i < out_n; ++i) {
        e += (double)out[i] * out[i];
    }
    CHECK(e / out_n > 0.5 * (12000.0 * 12000.0 / 2.0), "重采样后能量量级正确（>0.5×原均方）");

    /* 真实用法：整包重采样（Opus 解码每包 60ms；24k 包 = 1440 样本 → 960 输出） */
    mars_resample_init(&r, in_rate, out_rate);
    size_t total = 0;
    int16_t out2[2048];
    const size_t pkt24 = 1440;   /* 60ms@24k */
    for (size_t off = 0; off + pkt24 <= in_n; off += pkt24) {
        total += mars_resample_process(&r, in + off, pkt24, out2, 2048);
    }
    CHECK(total == 960 * (in_n / pkt24), "24k 整包重采样输出 960/包（精确）");

    /* 24k 包跨包边界：连续 3 包输出首尾无跳变（幅度差 < 2×峰值×比例系数） */
    mars_resample_init(&r, in_rate, out_rate);
    int16_t prev_last = 0;
    int smooth = 1;
    for (size_t off = 0; off + pkt24 <= in_n && off < pkt24 * 3; off += pkt24) {
        size_t n = mars_resample_process(&r, in + off, pkt24, out2, 2048);
        if (off > 0) {
            if (out2[0] > prev_last + 4000 || out2[0] < prev_last - 4000) {
                smooth = 0;
            }
        }
        prev_last = out2[n - 1];
    }
    CHECK(smooth, "跨包边界平滑（连续正弦，末/首样本差 < 4000）");

    /* 48k→16k：整包 60ms@48k = 2880 → 960 */
    mars_resample_init(&r, 48000, 16000);
    size_t n48 = mars_resample_process(&r, in, 2880, out, 2048);
    CHECK(n48 == 960, "48k→16k 整包 2880→960");

    /* 16k→16k 直通（重采样器仍可用，输出 = 输入） */
    mars_resample_init(&r, 16000, 16000);
    size_t n16 = mars_resample_process(&r, in, 960, out, 2048);
    CHECK(n16 == 960, "16k→16k 输出 960");

    /* 参数错误 */
    mars_resample_init(&r, 24000, 16000);
    CHECK(mars_resample_process(&r, NULL, 100, out, 100) == 0, "null 输入返回 0");
    CHECK(mars_resample_process(&r, in, 100, NULL, 100) == 0, "null 输出返回 0");
    CHECK(mars_resample_process(&r, in, 0, out, 100) == 0, "空输入返回 0");
    mars_resample_init(&r, 0, 16000);
    CHECK(r.initialized == 0, "init 0 采样率拒绝");
    free(in);
    free(out);
    printf("  通过\n");
}

/* ═══════════ 4. 复用循环（模拟 start/stop/close 反复） ═══════════ */

static void test_reuse_loop(void)
{
    printf("[复用循环] 反复 init/push/pull\n");
    static uint8_t mem[8 * 64];
    mars_pkt_queue_t q;
    mars_frame_acc_t acc;
    int16_t in[1280], out[960];
    for (int i = 0; i < 1280; ++i) {
        in[i] = (int16_t)(i * 3);
    }
    for (int round = 0; round < 50; ++round) {
        mars_pkt_queue_init(&q, mem, 64, 8);
        mars_frame_acc_init(&acc, 960);
        for (int i = 0; i < 3; ++i) {
            mars_frame_acc_push(&acc, in, 1280, NULL);
            uint16_t s = 1;
            while (mars_frame_acc_pull(&acc, out)) {
                char pkt[8];
                snprintf(pkt, sizeof(pkt), "f%d", i);
                CHECK(mars_pkt_queue_push(&q, pkt, 3, s++) == 0, "循环入队");
            }
        }
        char dst[64];
        size_t len = 0;
        while (mars_pkt_queue_pop(&q, dst, sizeof(dst), &len, NULL) == 0) {
            /* 全部取出 */
        }
        CHECK(mars_pkt_queue_count(&q) == 0, "循环后队列清空");
    }
    printf("  50 轮循环通过\n");
}

int main(void)
{
    printf("═══ mars_audio_stream host 单测 ═══\n");
    test_frame_acc();
    test_pkt_queue();
    test_resample();
    test_reuse_loop();
    printf("═══ 结果: PASS=%d FAIL=%d ═══\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
