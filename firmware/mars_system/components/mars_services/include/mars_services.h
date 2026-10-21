/**
 * @file mars_services.h
 * @brief 火星系统公共服务接口（应用层调用系统服务的统一入口）
 *
 * 所有应用（小智、未来的豆包/小爱同学等）通过这套接口调用系统能力，
 * 不直接访问硬件。这样应用可以独立编译、独立烧录、独立更新。
 */

#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

/* P1：Opus 流式音频 ABI（上行 16k/单声道/60ms 编码队列 + 下行解码播放）
 * 由 mars_audio_stream 组件提供实现；应用统一经本头文件调用。 */
#include "mars_audio_stream.h"
#include "mars_ws.h"
#include "mars_runtime.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ═══════════════════════════════════════════
 *  服务元信息（P1 新增）
 * ═══════════════════════════════════════════ */

/**
 * @brief 服务 ABI 版本（当前 1）
 */
uint32_t mars_services_api_version(void);

/**
 * @brief 能力位掩码（MARS_CAP_* 定义见 mars_audio_stream.h）
 */
uint64_t mars_services_capabilities(void);

/* 动态应用加载器在系统初始化时注册。资源指针在应用卸载前有效，
 * 服务必须在 mars_display_stop_emote() 返回后不再引用该指针。 */
typedef int (*mars_app_resource_provider_t)(const char *name, const void **data, size_t *size);
void mars_services_set_app_resource_provider(mars_app_resource_provider_t provider);
int mars_app_resource_get(const char *name, const void **data, size_t *size);

/* ═══════════════════════════════════════════
 *  音频服务
 * ═══════════════════════════════════════════ */

/**
 * @brief 开始录音
 * @return 0 成功，负数错误码
 */
int mars_audio_start_recording(void);

/**
 * @brief 停止录音
 * @return 0 成功
 */
int mars_audio_stop_recording(void);

/**
 * @brief 发送音频数据包到服务器
 * @param data 音频数据（Opus 编码）
 * @param len  数据长度
 * @return 0 成功
 */
int mars_audio_send_packet(const void* data, size_t len);

/**
 * @brief 播放音频数据包
 * @param data 音频数据（Opus 编码）
 * @param len  数据长度
 * @return 0 成功
 */
int mars_audio_play_packet(const void* data, size_t len);

/**
 * @brief 设置音量
 * @param volume 0-100
 * @return 0 成功
 */
int mars_audio_set_volume(int volume);


/* ═══════════════════════════════════════════
 *  网络服务
 * ═══════════════════════════════════════════ */

/**
 * @brief Wi-Fi 连接状态
 */
typedef enum {
    MARS_NET_DISCONNECTED = 0,
    MARS_NET_CONNECTING,
    MARS_NET_CONNECTED,
    MARS_NET_FAILED,
} mars_net_state_t;

/**
 * @brief 获取 Wi-Fi 状态
 */
mars_net_state_t mars_network_get_state(void);

/**
 * @brief 是否已保存过 Wi-Fi 配置
 */
bool mars_network_has_saved(void);

/**
 * @brief 通过 MQTT 发布消息
 * @param topic   主题
 * @param payload 消息内容
 * @param len     消息长度
 * @return 0 成功
 */
int mars_network_mqtt_publish(const char* topic, const void* payload, size_t len);


/* ═══════════════════════════════════════════
 *  显示服务
 * ═══════════════════════════════════════════ */

/**
 * @brief 绘制 UI 布局（JSON 格式）
 * @param layout_json 布局描述
 * @return 0 成功
 */
int mars_display_draw_layout(const char* layout_json);

/**
 * @brief 播放表情动画
 * @param emote_name 表情名字（如 "listening", "thinking"）
 * @return 0 成功
 */
int mars_display_play_emote(const char* emote_name);

/**
 * @brief 停止表情动画
 */
int mars_display_stop_emote(void);

/**
 * @brief 把表情引擎的最新帧合成到系统 framebuffer
 * @return 1 有可用帧，0 暂无新帧，负数为错误
 */
int mars_display_render_emote(void);


/* ═══════════════════════════════════════════
 *  唤醒服务
 * ═══════════════════════════════════════════ */

/**
 * @brief 唤醒词检测回调
 * @param wake_word 检测到的唤醒词
 */
typedef void (*mars_wakeword_callback_t)(const char* wake_word);

/**
 * @brief 注册唤醒词检测回调
 * @param cb 回调函数
 * @return 0 成功
 */
int mars_wakeword_set_callback(mars_wakeword_callback_t cb);

/**
 * @brief 启用唤醒词检测
 */
int mars_wakeword_enable(void);

/**
 * @brief 禁用唤醒词检测
 */
int mars_wakeword_disable(void);


/* ═══════════════════════════════════════════
 *  系统服务
 * ═══════════════════════════════════════════ */

/**
 * @brief 获取系统运行时间（毫秒）
 */
int64_t mars_system_get_uptime_ms(void);
void mars_system_delay_ms(uint32_t milliseconds);

/**
 * @brief 重启到指定应用位
 * @param slot 应用位编号（0/1/2）
 * @return 不返回（成功时重启）
 */
int mars_system_restart_to_app(int slot);

/**
 * @brief 退出当前应用，回到桌面
 */
int mars_system_exit_to_desktop(void);

#ifdef __cplusplus
}
#endif
