#!/usr/bin/env python3
"""
导出系统符号表（动态链接基础）

从固件 ELF（xiaozhi.elf / mars.elf）导出全局符号，生成 mars_symtab.c，
供应用加载器解析应用的未定义符号。

用法：
    python3 export_symtab.py <elf_path> <output_c_path>

符号选择规则：
    - 只导出 T/D/R/B 等已定义符号（跳过 U 未定义）
    - 过滤：不导出内部局部符号、debug 符号
    - 可选白名单（--whitelist）：只导出需要的符号（更安全、更小）
"""

import argparse
import re
import subprocess
from pathlib import Path

# 应用可能调用的系统符号（白名单模式）
DEFAULT_WHITELIST = [
    "__divdi3", "__moddi3", "__bswapsi2",
    "mars_services_api_version", "mars_services_capabilities",
    "mars_audio_stream_start", "mars_audio_stream_read", "mars_audio_stream_stop", "mars_audio_stream_finish",
    "mars_audio_play_opus", "mars_audio_playback_stop", "mars_audio_playback_pending",
    "mars_audio_stream_stats", "mars_audio_stream_is_running",
    "mars_ws_saved_config", "mars_ws_open_saved", "mars_ws_send_text",
    "mars_ws_send_binary", "mars_ws_receive", "mars_ws_close",
    "mars_app_resource_get", "mars_system_delay_ms",
    "cJSON_ParseWithLength", "cJSON_GetObjectItemCaseSensitive", "cJSON_IsString",
    "cJSON_IsNumber", "cJSON_Delete", "cJSON_GetArraySize", "cJSON_GetArrayItem",
    # libc
    "malloc", "free", "calloc", "realloc",
    "memcpy", "memset", "memmove", "memcmp", "memchr",
    "strlen", "strcmp", "strncmp", "strcpy", "strncpy",
    "strcat", "strncat", "strchr", "strrchr", "strstr",
    "strtol", "strtoul", "strtoll", "strtoull",
    "sprintf", "snprintf", "vsnprintf", "printf", "puts",
    "abs", "atoi", "atol", "atof", "isalpha", "isdigit",
    "rand", "srand", "qsort", "bsearch",
    # 火星系统服务
    "mars_audio_start_recording", "mars_audio_stop_recording",
    "mars_audio_send_packet", "mars_audio_play_packet", "mars_audio_set_volume",
    "mars_network_get_state", "mars_network_has_saved", "mars_network_mqtt_publish",
    "mars_display_draw_layout", "mars_display_play_emote", "mars_display_stop_emote",
    "mars_display_render_emote",
    "mars_wakeword_set_callback", "mars_wakeword_enable", "mars_wakeword_disable",
    "mars_system_get_uptime_ms", "mars_system_restart_to_app", "mars_system_exit_to_desktop",
    # ESP-IDF 常用
    "esp_log_write", "esp_log_level_set",
    "vTaskDelay", "vTaskDelayUntil", "xTaskCreate", "xTaskCreatePinnedToCore",
    "esp_timer_get_time",
    # gfx 屏幕绘制（动态应用可见反馈）
    "gfx_clear", "gfx_present", "gfx_rect", "gfx_text", "gfx_text_overlay", "gfx_text_width", "mars_glyph_copy",
]


def find_nm():
    """查找 xtensa nm 工具"""
    # 尝试 PATH
    for name in ("xtensa-esp32s3-elf-nm", "xtensa-esp-elf-nm"):
        import shutil
        p = shutil.which(name)
        if p:
            return p
    raise FileNotFoundError("xtensa nm not found, pass --nm")


def main():
    ap = argparse.ArgumentParser(description="导出系统符号表")
    ap.add_argument("elf", type=Path, help="固件 ELF 路径")
    ap.add_argument("output", type=Path, help="输出 C 文件路径")
    ap.add_argument("--nm", type=str, default=None, help="nm 工具路径")
    ap.add_argument("--whitelist", action="store_true",
                    help="只导出白名单符号（推荐）")
    args = ap.parse_args()

    nm = args.nm or find_nm()

    # 运行 nm 获取符号
    result = subprocess.run(
        [nm, "--defined-only", "-n", str(args.elf)],
        capture_output=True, text=True, check=True)

    # 解析符号：格式 "地址 类型 名称"
    symbols = []  # (name, addr)
    skipped = 0
    for line in result.stdout.splitlines():
        parts = line.split()
        if len(parts) < 3:
            continue
        addr_str, sym_type, name = parts[0], parts[1], parts[2]

        # 类型过滤：T/R/D/B 等已定义符号
        if sym_type not in "TtRrDdBbAa":
            continue
        # 跳过内部命名（gcc 生成的）
        if name.startswith("."):
            continue
        # 跳过 C++ mangled 符号（暂时只导 C 符号）
        if name.startswith("_Z"):
            skipped += 1
            continue

        try:
            addr = int(addr_str, 16)
        except ValueError:
            continue
        if addr == 0:
            continue

        # 白名单过滤
        if args.whitelist and name not in DEFAULT_WHITELIST:
            skipped += 1
            continue

        symbols.append((name, addr))

    # 去重（按名字保留第一个）
    seen = set()
    uniq = []
    for name, addr in symbols:
        if name in seen:
            continue
        seen.add(name)
        uniq.append((name, addr))

    # mars_services 服务符号：通过取地址强引用，防止 --gc-sections 裁剪，
    # 并让链接器把服务函数地址填进符号表。
    MARS_SERVICE_SYMBOLS = [
        "mars_audio_start_recording", "mars_audio_stop_recording",
        "mars_audio_send_packet", "mars_audio_play_packet", "mars_audio_set_volume",
        "mars_network_get_state", "mars_network_has_saved", "mars_network_mqtt_publish",
        "mars_display_draw_layout", "mars_display_play_emote", "mars_display_stop_emote",
        "mars_display_render_emote",
        "mars_wakeword_set_callback", "mars_wakeword_enable", "mars_wakeword_disable",
        "mars_system_get_uptime_ms", "mars_system_restart_to_app", "mars_system_exit_to_desktop",
    ]
    service_refs = [s for s in MARS_SERVICE_SYMBOLS if s not in seen]

    # 生成 C 文件
    lines = []
    lines.append("/**")
    lines.append(" * @file mars_symtab.c")
    lines.append(" * @brief 系统导出符号表（由 tools/export_symtab.py 自动生成）")
    lines.append(" *")
    lines.append(f" * 来源: {args.elf.name}")
    lines.append(f" * 生成时间: {__import__('datetime').datetime.now().strftime('%Y-%m-%d %H:%M:%S')}")
    lines.append(f" * 符号数: {len(uniq) + len(service_refs)}")
    lines.append(" */")
    lines.append("")
    lines.append('#include "mars_app_loader.h"')
    lines.append('#include "mars_services.h"')
    lines.append("")
    lines.append("/* 强引用：确保 mars_services 实现保留在最终固件 ELF 中（供动态应用解析） */")
    lines.append("static const void * const mars_services_keep[] = {")
    for s in service_refs:
        lines.append(f"    (const void *){s},")
    lines.append("};")
    lines.append("/* 防止 keep 数组被当作未使用（链接器仍可见引用，编译器不再警告） */")
    lines.append("const void* mars_symtab_service_keep(void) { return (const void*)mars_services_keep; }")
    lines.append("")
    lines.append("const mars_symtab_entry_t mars_symtab[] = {")
    for name, addr in uniq:
        lines.append(f'    {{ "{name}", 0x{addr:08x}u }},')
    for s in service_refs:
        lines.append(f'    {{ "{s}", (uint32_t)(uintptr_t){s} }},')
    lines.append("};")
    lines.append("")
    lines.append(f"const uint32_t mars_symtab_count = {len(uniq) + len(service_refs)};")
    lines.append("")
    lines.append("/* 结束 */")
    lines.append("")

    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text("\n".join(lines), encoding="utf-8")

    print(f"✅ 符号表已生成: {args.output}")
    print(f"   导出符号: {len(uniq)}，跳过: {skipped}")
    if args.whitelist:
        print(f"   （白名单模式）")


if __name__ == "__main__":
    main()
