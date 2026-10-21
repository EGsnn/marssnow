#pragma once

/* 小智应用桥接层
 * 阶段 1：代码结构解耦完成
 * 未来迁移方向：实现将移至 apps/xiaozhi/ 目录
 * 接口规范见 apps/xiaozhi/include/xiaozhi_app.h
 */

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

void mars_xiaozhi_open(void);
void mars_xiaozhi_close(void);
bool mars_xiaozhi_render(bool force);
void mars_xiaozhi_tap(void);

#ifdef __cplusplus
}
#endif
