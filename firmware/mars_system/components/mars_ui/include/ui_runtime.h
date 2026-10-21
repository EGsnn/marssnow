#pragma once

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>

typedef bool (*ui_runtime_action_handler_t)(const char *action_id);

void ui_runtime_init(void);
/* Load protocol, document and shared font without presenting the JSON desktop. */
void ui_runtime_init_suspended(void);
void ui_runtime_set_action_handler(ui_runtime_action_handler_t handler);
/* 注册音频自测命令处理器（由 main 注入 mars_audio_stream_selftest）。
 * 串口收到 @AUDIO:SELFTEST 行时在该回调内执行；回调返回 0=PASS，
 * 非0=FAIL 且 reason 填入原因，串口层据此输出 PASS/FAIL:<reason>。 */
void ui_runtime_set_selftest_handler(int (*handler)(char *reason, size_t reason_len));
bool ui_runtime_poll(void);
bool ui_runtime_is_active(void);
void ui_runtime_suspend(void);
void ui_runtime_resume(void);
void ui_runtime_handle_touch(bool down, uint16_t x, uint16_t y);
