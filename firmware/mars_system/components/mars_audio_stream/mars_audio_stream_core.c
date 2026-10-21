/* mars_audio_stream_core：拼帧器 + 有界包队列（纯 C，可主机侧测试）
 *
 * 注意：本文件只提供数据结构与无锁逻辑；多生产者/消费者的互斥
 * （编码任务 push vs 应用 read/pop 等）由固件层（mars_audio_stream.c）
 * 在调用本文件前加锁，不在此文件内假设任何线程模型。
 */

#include "mars_audio_stream_core.h"

#include <string.h>

/* ═══════════ 拼帧器 ═══════════ */

void mars_frame_acc_init(mars_frame_acc_t *acc, size_t frame_samples)
{
    if (acc == NULL) {
        return;
    }
    if (frame_samples == 0 || frame_samples > MARS_FRAME_ACC_MAX_SAMPLES) {
        frame_samples = MARS_FRAME_ACC_MAX_SAMPLES;
    }
    acc->n = 0;
    acc->frame_samples = frame_samples;
}

void mars_frame_acc_push(mars_frame_acc_t *acc, const int16_t *data, size_t n,
                         size_t *dropped)
{
    if (acc == NULL || data == NULL || n == 0) {
        return;
    }
    if (n > MARS_FRAME_ACC_MAX_SAMPLES - acc->n) {
        /* 超限：丢弃最旧样本，腾出空间 */
        size_t need = n - (MARS_FRAME_ACC_MAX_SAMPLES - acc->n);
        if (need > acc->n) {
            need = acc->n;
        }
        if (need > 0) {
            memmove(acc->buf, acc->buf + need, (acc->n - need) * sizeof(int16_t));
            acc->n -= need;
            if (dropped != NULL) {
                *dropped += need;
            }
        }
    }
    size_t room = MARS_FRAME_ACC_MAX_SAMPLES - acc->n;
    size_t take = n < room ? n : room;
    memcpy(acc->buf + acc->n, data, take * sizeof(int16_t));
    acc->n += take;
    if (take < n && dropped != NULL) {
        *dropped += n - take;
    }
}

int mars_frame_acc_pull(mars_frame_acc_t *acc, int16_t *out)
{
    if (acc == NULL || out == NULL || acc->n < acc->frame_samples) {
        return 0;
    }
    memcpy(out, acc->buf, acc->frame_samples * sizeof(int16_t));
    memmove(acc->buf, acc->buf + acc->frame_samples,
            (acc->n - acc->frame_samples) * sizeof(int16_t));
    acc->n -= acc->frame_samples;
    return 1;
}

size_t mars_frame_acc_available(const mars_frame_acc_t *acc)
{
    return acc == NULL ? 0 : acc->n;
}

int mars_frame_acc_has_frame(const mars_frame_acc_t *acc)
{
    return acc != NULL && acc->n >= acc->frame_samples;
}

/* ═══════════ 有界包队列 ═══════════ */

int mars_pkt_queue_init(mars_pkt_queue_t *q, void *mem, size_t slot_size, uint16_t depth)
{
    if (q == NULL || mem == NULL || slot_size <= MARS_PKT_HDR_SIZE ||
        depth == 0 || depth > MARS_PKT_QUEUE_MAX_DEPTH) {
        return -1;
    }
    q->slots     = (uint8_t *)mem;
    q->slot_size = slot_size;
    q->depth     = depth;
    q->head      = 0;
    q->count     = 0;
    q->dropped   = 0;
    q->overflows = 0;
    return 0;
}

static size_t pkt_len_at(const mars_pkt_queue_t *q, uint16_t idx)
{
    const uint8_t *p = q->slots + (size_t)idx * q->slot_size;
    return (size_t)p[0] | ((size_t)p[1] << 8);
}

int mars_pkt_queue_push(mars_pkt_queue_t *q, const void *pkt, size_t len, uint16_t seq)
{
    if (q == NULL || q->slots == NULL || pkt == NULL || len == 0) {
        if (q != NULL) {
            q->overflows++;
        }
        return -1;
    }
    if (len > q->slot_size - MARS_PKT_HDR_SIZE) {
        q->overflows++;
        return -1;
    }
    if (q->count == q->depth) {
        /* 满：丢弃最旧包 */
        ++q->dropped;
        --q->count;
        ++q->head;
        if (q->head >= q->depth) {
            q->head = 0;
        }
    }
    uint16_t tail = (uint16_t)((q->head + q->count) % q->depth);
    uint8_t *dst = q->slots + (size_t)tail * q->slot_size;
    dst[0] = (uint8_t)(len & 0xFF);
    dst[1] = (uint8_t)((len >> 8) & 0xFF);
    dst[2] = (uint8_t)(seq & 0xFF);
    dst[3] = (uint8_t)((seq >> 8) & 0xFF);
    memcpy(dst + MARS_PKT_HDR_SIZE, pkt, len);
    ++q->count;
    return 0;
}

int mars_pkt_queue_pop(mars_pkt_queue_t *q, void *dst, size_t cap, size_t *out_len,
                       uint16_t *out_seq)
{
    if (q == NULL || q->slots == NULL || dst == NULL) {
        return -1;
    }
    if (q->count == 0) {
        return -1;   /* 空 */
    }
    size_t len = pkt_len_at(q, q->head);
    if (len > cap) {
        return -2;   /* 目的缓冲过小（不消费） */
    }
    const uint8_t *src = q->slots + (size_t)q->head * q->slot_size;
    if (out_seq != NULL) {
        *out_seq = (uint16_t)src[2] | ((uint16_t)src[3] << 8);
    }
    memcpy(dst, src + MARS_PKT_HDR_SIZE, len);
    --q->count;
    ++q->head;
    if (q->head >= q->depth) {
        q->head = 0;
    }
    if (out_len != NULL) {
        *out_len = len;
    }
    return 0;
}

void mars_pkt_queue_reset(mars_pkt_queue_t *q)
{
    if (q == NULL) {
        return;
    }
    q->head  = 0;
    q->count = 0;
    /* dropped/overflows 累计保留，便于统计全生命周期 */
}

uint16_t mars_pkt_queue_count(const mars_pkt_queue_t *q)
{
    return q == NULL ? 0 : q->count;
}

int mars_pkt_queue_full(const mars_pkt_queue_t *q)
{
    return q != NULL && q->count == q->depth;
}

uint32_t mars_pkt_queue_dropped(const mars_pkt_queue_t *q)
{
    return q == NULL ? 0 : q->dropped;
}
