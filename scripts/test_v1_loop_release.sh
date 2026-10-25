#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"
CXX="${CXX:-g++}"
OUT="${V1_RELEASE_BUILD_DIR:-build/v1-loop-release}"
mkdir -p "$OUT"
FLAGS=(-std=c++17 -O2 -Wall -Wextra -Werror -pedantic -Ifirmware)
if [[ "${SANITIZE:-0}" == 1 ]]; then
  FLAGS+=(-O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer)
fi
"$CXX" "${FLAGS[@]}" tests/voice/test_loop_crossfade_release.cpp -o "$OUT/test_release"
"$OUT/test_release"
"$CXX" "${FLAGS[@]}" tools/loop_release_probe.cpp -o "$OUT/loop_release_probe"
export V1_RELEASE_PROBE="$(cd "$OUT" && pwd)/loop_release_probe"
python3 -m unittest discover -s tests/voice -p 'test_loop_release_oracle.py' -v
