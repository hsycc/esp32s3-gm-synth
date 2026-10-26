#!/usr/bin/env bash
set -euo pipefail
cd "$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
OUT="${V1_APP_TARGET_DIR:-build/v1-loop-app-target}"
python3 tests/app/generate_loop_app_embedded.py "$OUT/generated"
FLAGS=(-std=c++17 -O2 -Wall -Wextra -Werror -pedantic -pthread -Ifirmware -I"$OUT/generated" -DGM_SYNTH_EXPERIMENTAL_VOICE_LOOP=1)
if [[ "${SANITIZE:-0}" == 1 ]]; then FLAGS+=(-O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer); fi
SOURCES=(firmware/sample/*.cpp firmware/gm/*.cpp firmware/sf2/*.cpp firmware/voice/*.cpp firmware/realtime/*.cpp)
"${CXX:-g++}" "${FLAGS[@]}" "${SOURCES[@]}" tests/app/test_loop_app_target_native.cpp -lcrypto -o "$OUT/native"
"$OUT/native"
