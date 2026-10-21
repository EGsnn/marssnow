#!/usr/bin/env bash
# P1 主机离线测试统一入口（第四轮）
# - 全部产物写入临时目录，不污染仓库
# - -Wall -Wextra -Werror -fsanitize=address,undefined
# - 核心 / 服务 / line discipline 各连续跑 3 次，任一失败立即非零退出
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
AUDIO_DIR="$(cd "$HERE/.." && pwd)"            # components/mars_audio_stream
UI_DIR="$(cd "$AUDIO_DIR/../mars_ui" && pwd)"  # components/mars_ui
MEDIA_DIR="$(cd "$AUDIO_DIR/../mars_media" && pwd)"

OUT="$(mktemp -d -t mars_p1_tests.XXXXXX)"
trap 'rm -rf "$OUT"' EXIT
echo "== 产物目录: $OUT =="

CC="${CC:-clang}"
SAN="-fsanitize=address,undefined -g"
WARN="-std=c11 -Wall -Wextra -Werror"

$CC $WARN $SAN -I "$AUDIO_DIR/include" \
    "$AUDIO_DIR/host_test/test_voice_effect.c" "$AUDIO_DIR/mars_voice_effect.c" -o "$OUT/test_voice"
"$OUT/test_voice"

# ── 1) 核心单测（pkt queue / frame acc / resample） ──
echo "== 构建核心测试 =="
$CC $WARN $SAN -I "$AUDIO_DIR/include" \
    "$AUDIO_DIR/host_test/test_host.c" \
    "$AUDIO_DIR/mars_audio_stream_core.c" \
    "$AUDIO_DIR/mars_resample.c" \
    "$AUDIO_DIR/mars_voice_effect.c" \
    -o "$OUT/test_host"

# ── 2) 服务测试（生命周期 / 并发 / 故障注入 / 所有权） ──
echo "== 构建服务测试 =="
$CC $WARN $SAN \
    -I "$AUDIO_DIR/include" \
    -I "$AUDIO_DIR/host_test/stub" \
    -I "$MEDIA_DIR/include" \
    -DMARS_HOST_TEST \
    "$AUDIO_DIR/host_test/test_service.c" \
    "$AUDIO_DIR/host_test/stub/FreeRTOS_stub.c" \
    "$AUDIO_DIR/host_test/stub/codec_stub.c" \
    "$AUDIO_DIR/host_test/stub/media_stub.c" \
    "$AUDIO_DIR/host_test/stub/host_malloc.c" \
    "$AUDIO_DIR/mars_audio_stream.c" \
    "$AUDIO_DIR/mars_audio_stream_core.c" \
    "$AUDIO_DIR/mars_resample.c" \
    "$AUDIO_DIR/mars_voice_effect.c" \
    "$MEDIA_DIR/output_owner.c" \
    -lpthread \
    -o "$OUT/test_service"

# ── 3) line discipline 测试（含超长行整行丢弃对抗用例） ──
echo "== 构建 line discipline 测试 =="
$CC $WARN $SAN -I "$UI_DIR/include" \
    "$UI_DIR/host_test/test_line_disc.c" \
    "$UI_DIR/line_disc.c" \
    -o "$OUT/test_line_disc"

# ── 连续 3 次 ──
for i in 1 2 3; do
    echo "── 第 $i/3 轮 ──"
    echo "  [core]"
    "$OUT/test_host" | sed 's/^/    /'
    echo "  [service]"
    "$OUT/test_service" > "$OUT/svc.log" 2>&1 || { cat "$OUT/svc.log"; exit 1; }
    tail -n 1 "$OUT/svc.log" | sed 's/^/    /'
    echo "  [line_disc]"
    "$OUT/test_line_disc" | tail -n 1 | sed 's/^/    /'
done

echo "== 全部 3 轮通过 =="

# ── 4) ThreadSanitizer：并发生命周期/统计/测试桩不得有数据竞争 ──
echo "== 构建并运行 ThreadSanitizer 服务测试 =="
$CC $WARN -fsanitize=thread -g \
    -I "$AUDIO_DIR/include" \
    -I "$AUDIO_DIR/host_test/stub" \
    -I "$MEDIA_DIR/include" \
    -DMARS_HOST_TEST \
    "$AUDIO_DIR/host_test/test_service.c" \
    "$AUDIO_DIR/host_test/stub/FreeRTOS_stub.c" \
    "$AUDIO_DIR/host_test/stub/codec_stub.c" \
    "$AUDIO_DIR/host_test/stub/media_stub.c" \
    "$AUDIO_DIR/host_test/stub/host_malloc.c" \
    "$AUDIO_DIR/mars_audio_stream.c" \
    "$AUDIO_DIR/mars_audio_stream_core.c" \
    "$AUDIO_DIR/mars_resample.c" \
    "$AUDIO_DIR/mars_voice_effect.c" \
    "$MEDIA_DIR/output_owner.c" \
    -lpthread \
    -o "$OUT/test_service_tsan"
TSAN_OPTIONS="halt_on_error=1 exitcode=66" \
    "$OUT/test_service_tsan" > "$OUT/tsan.log" 2>&1 || { cat "$OUT/tsan.log"; exit 1; }
tail -n 1 "$OUT/tsan.log" | sed 's/^/    /'
echo "== ThreadSanitizer 通过 =="
