#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")"
ROOT="$(cd ../../.. && pwd)"
OUT="$(mktemp -d -t mars-cloud-test.XXXXXX)"
trap 'rm -rf "$OUT"' EXIT
clang -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined \
  -DMARS_HOST_TEST -Istub -I../include -I../../mars_audio_stream/host_test/stub \
  test_app_preferences.c ../src/mars_app_preferences.c \
  ../../mars_audio_stream/host_test/stub/FreeRTOS_stub.c -lpthread -o "$OUT/test_preferences"
"$OUT/test_preferences"

clang -std=c11 -Wall -Wextra -Werror -fsanitize=thread \
  -DMARS_HOST_TEST -Istub -I../include -I../../mars_audio_stream/host_test/stub \
  test_app_preferences.c ../src/mars_app_preferences.c \
  ../../mars_audio_stream/host_test/stub/FreeRTOS_stub.c -lpthread -o "$OUT/test_preferences_tsan"
TSAN_OPTIONS="halt_on_error=1 exitcode=66" "$OUT/test_preferences_tsan"
python3 run_ws_config_tests.py
