#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
CXX="${CXX:-g++}"
OUT="${V1_LIFETIME_BUILD_DIR:-build/v1-loop-lifetime}"
mkdir -p "$OUT"
FLAGS=(-std=c++17 -O2 -Wall -Wextra -Werror -pedantic -pthread -Ifirmware)
if [[ "${SANITIZE:-0}" == 1 ]]; then
  FLAGS+=(-O1 -g -fno-omit-frame-pointer -fsanitize=address,undefined -fno-sanitize-recover=all)
fi
"$CXX" "${FLAGS[@]}" tests/voice/test_loop_crossfade_lifetime.cpp \
  firmware/sample/PublishedSampleDirectory.cpp -o "$OUT/test_loop_lifetime"
"$OUT/test_loop_lifetime"
"$CXX" "${FLAGS[@]}" tools/loop_pinned_probe.cpp \
  firmware/sample/PublishedSampleDirectory.cpp -o "$OUT/loop_pinned_probe"
