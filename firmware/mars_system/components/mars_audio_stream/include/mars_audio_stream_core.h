#pragma once

/* mars_audio_stream_core：火星系统 Opus 流式音频服务的纯 C 核心
 *
 * 两个可独立测试的部件：
 *   1. 拼帧器（mars_frame_acc）：把底层任意长度 PCM 帧（如 80 ms / 1280
 *      样本）累积并切分成固定长度编码帧（如 60 ms / 960 样本 @16 kHz）。
 *   2. 有界包队列（mars_pkt_queue）：定长槽位环形队列，存放 Opus 包；
 *      满时丢弃最旧包并计数（保持实时性）。
 *
 * 无 FreeRTOS / ESP-IDF 依赖，可主机侧单元测试。
 */

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ═══════════ 拼帧器 ═══════════ */

#define MARS_FRAME_ACC_MAX_SAMPLES 4800   /* 最大累积样本（300 ms @16k），防止长期欠采样时无限增长 */

typedef struct {
    int16_t buf[MARS_FRAME_ACC_MAX_SAMPLES];
    size_t  n;            /* 已累积样本数 */
    size_t  frame_samples; /* 目标帧样本数（如 960） */
} mars_frame_acc_t;

/* 初始化拼帧器，frame_samples 为目标帧长度（>0 且 <= MAX）。 */
void mars_frame_acc_init(mars_frame_acc_t *acc, size_t frame_samples);

/* 推入一段 PCM（单声道 int16）。若累积超过上限，丢弃最旧部分并计数。
 * @param dropped 非空时累加因超限丢弃的样本数。 */
void mars_frame_acc_push(mars_frame_acc_t *acc, const int16_t *data, size_t n,
                         size_t *dropped);

/* 取出一整帧（frame_samples 个样本）。
 * @param out 容量至少 frame_samples 的缓冲
 * @return 1 表示取出一帧，0 表示累积不足。 */
int mars_frame_acc_pull(mars_frame_acc_t *acc, int16_t *out);

/* 当前累积样本数。 */
size_t mars_frame_acc_available(const mars_frame_acc_t *acc);

/* 是否已累积满一帧。 */
int mars_frame_acc_has_frame(const mars_frame_acc_t *acc);

/* ═══════════ 有界包队列 ═══════════ */

#define MARS_PKT_QUEUE_MAX_DEPTH 32

/* 包在槽内的布局：前 4 字节 = 长度(2B 小端) + 包序号(2B 小端)，其后为负载。 */
#define MARS_PKT_HDR_SIZE 4

typedef struct {
    uint8_t *slots;         /* 外部提供的定长槽位内存（深度 × 槽长） */
    size_t   slot_size;     /* 单槽字节数 */
    uint16_t depth;         /* 槽数 */
    uint16_t head;          /* 读指针（最旧） */
    uint16_t count;         /* 当前包数 */
    uint32_t dropped;       /* 因队列满丢弃的包数（丢最旧） */
    uint32_t overflows;     /* push 因参数/容量失败的计数 */
} mars_pkt_queue_t;

/* 初始化队列。mem 为外部提供的内存（深度 × slot_size 字节），
 * slot_size 需 >= MARS_PKT_HDR_SIZE，depth <= MARS_PKT_QUEUE_MAX_DEPTH。
 * @return 0 成功，-1 参数错误。 */
int mars_pkt_queue_init(mars_pkt_queue_t *q, void *mem, size_t slot_size, uint16_t depth);

/* 入队一个包。len <= slot_size - HDR 时成功；队列满则丢弃最旧包
 * （dropped++），新包入队。
 * @param seq 包序号（逐包唯一；与负载一起存储，出队时原样返回）
 * @return 0 成功，-1 参数/容量错误。 */
int mars_pkt_queue_push(mars_pkt_queue_t *q, const void *pkt, size_t len, uint16_t seq);

/* 出队一个包（FIFO，最旧优先）。
 * @param dst 目的缓冲，cap 为其容量
 * @param out_len 非空时返回实际包长
 * @param out_seq 非空时返回包序号
 * @return 0 成功；-1 队列空（EAGAIN 语义）；-2 目的缓冲过小（ENOMEM 语义，
 *         队列不消费）。 */
int mars_pkt_queue_pop(mars_pkt_queue_t *q, void *dst, size_t cap, size_t *out_len,
                       uint16_t *out_seq);

/* 清空队列（丢弃所有包，不重置累计 dropped/overflows 计数）。 */
void mars_pkt_queue_reset(mars_pkt_queue_t *q);

/* 队列当前包数。 */
uint16_t mars_pkt_queue_count(const mars_pkt_queue_t *q);

/* 队列是否满。 */
int mars_pkt_queue_full(const mars_pkt_queue_t *q);

/* 累计丢弃包数。 */
uint32_t mars_pkt_queue_dropped(const mars_pkt_queue_t *q);

#ifdef __cplusplus
}
#endif
