#!/usr/bin/env bash
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
OUT="$ROOT/build/examples/hello_mars"
SYSTEM="$ROOT/firmware/mars_system"
GCC="${MARS_CC:-xtensa-esp32s3-elf-gcc}"
LD="${MARS_LD:-xtensa-esp32s3-elf-ld}"
mkdir -p "$OUT"
"$GCC" -Os -std=c11 -Wall -Wextra -Werror -fPIC -mlongcalls -mtext-section-literals \
    -ffunction-sections -fdata-sections -fno-jump-tables -fno-tree-switch-conversion -nostdlib \
    -I"$SYSTEM/components/mars_common/include" \
    -I"$SYSTEM/components/mars_services/include" \
    -I"$SYSTEM/components/mars_audio_stream/include" \
    -c "$HERE/app.c" -o "$OUT/app.o"
"$LD" -r "$OUT/app.o" -o "$OUT/hello_mars.o"
python3 "$SYSTEM/components/mars_app_loader/tools/pack_marsapp.py" \
    "$OUT/hello_mars.o" "$OUT/hello_mars.marsapp" --name hello_mars \
    --entry app_entry --stack 8192 --heap 2048
python3 "$SYSTEM/components/mars_app_loader/tools/verify_package.py" \
    "$OUT/hello_mars.marsapp" "$SYSTEM/components/mars_app_loader/src/mars_exports_def.c"
