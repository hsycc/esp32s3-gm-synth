#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
CXX="${CXX:-g++}"
OUT="${V1_CURSOR_BUILD_DIR:-build/v1-loop-cursor}"
mkdir -p "$OUT"
FLAGS=(-std=c++17 -O2 -Wall -Wextra -Werror -pedantic -Ifirmware)
if [[ "${SANITIZE:-0}" == 1 ]]; then
  FLAGS+=(-O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer)
fi
"$CXX" "${FLAGS[@]}" tests/voice/test_loop_crossfade_cursor.cpp -o "$OUT/test_cursor"
"$OUT/test_cursor"
"$CXX" "${FLAGS[@]}" -pthread firmware/sample/PublishedSampleDirectory.cpp \
  firmware/voice/VoiceRenderer.cpp tests/voice/test_loop_crossfade_voice_parity.cpp \
  -o "$OUT/test_voice_parity"
"$OUT/test_voice_parity"
"$CXX" "${FLAGS[@]}" tools/loop_cursor_probe.cpp -o "$OUT/loop_cursor_probe"
V1_LOOP_CURSOR_PROBE="$(cd "$OUT" && pwd)/loop_cursor_probe" \
  python3 -m unittest discover -s tests -p 'test_v1_loop_executor_check.py' -v
