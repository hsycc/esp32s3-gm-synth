#!/usr/bin/env bash
set -euo pipefail
cd "$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
OUT="${V1_APP_BUILD_DIR:-build/v1-loop-app}"
mkdir -p "$OUT"
python3 tests/app/make_loop_app_fixture.py "$OUT/fixture.sf2" | tee "$OUT/fixture.sha256"
FLAGS=(-std=c++17 -O2 -Wall -Wextra -Werror -pedantic -pthread -Ifirmware -DGM_SYNTH_EXPERIMENTAL_VOICE_LOOP=1)
if [[ "${SANITIZE:-0}" == 1 ]]; then FLAGS+=(-O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer); fi
SOURCES=(firmware/sample/*.cpp firmware/gm/*.cpp firmware/sf2/*.cpp firmware/voice/*.cpp firmware/realtime/*.cpp)
"${CXX:-g++}" "${FLAGS[@]}" "${SOURCES[@]}" tests/app/test_loop_program_runtime.cpp -o "$OUT/test_app"
"$OUT/test_app" "$OUT/fixture.sf2"
