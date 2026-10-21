#pragma once

#include <stddef.h>

/* mars_audio_stream 固件内自测钩子（供实机/集成验证执行）
 *
 * 离线阶段仅编译验证；运行留待 Codex 实机验收：
 *   固定 60ms@16k 正弦 PCM → Opus 编码 → Opus 解码 →
 *   校验输出能量/长度，结果通过 ESP_LOGI 打印。
 */

#ifdef __cplusplus
extern "C" {
#endif

/* 在独立大栈任务中执行 Opus 编解码往返自测（不改变服务状态）。
 * @param reason 非空时，失败填入简短原因；成功保持空串。
 * @return 0=PASS；非0=FAIL（reason 已说明）。串口层据此输出
 *         @AUDIO:SELFTEST:PASS 或 @AUDIO:SELFTEST:FAIL:<reason>。 */
int mars_audio_stream_selftest(char *reason, size_t reason_len);

#ifdef __cplusplus
}
#endif
