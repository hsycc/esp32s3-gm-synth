#!/usr/bin/env bash
set -euo pipefail
cd "$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
OUT="${V1_REALTIME_DIR:-build/v1-loop-realtime-host}"
mkdir -p "$OUT"
python3 tests/app/make_loop_analysis_fixture.py "$OUT/generated"
FLAGS=(-std=c++17 -O2 -Wall -Wextra -Werror -pedantic -pthread -Ifirmware -I"$OUT/generated" -DGM_SYNTH_EXPERIMENTAL_VOICE_LOOP=1)
if [[ "${SANITIZE:-0}" == 1 && "${THREAD_SANITIZE:-0}" == 1 ]]; then
    echo 'ASan/UBSan and ThreadSanitizer require separate builds' >&2
    exit 2
fi
if [[ "${SANITIZE:-0}" == 1 ]]; then
    FLAGS+=(-O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer)
fi
if [[ "${THREAD_SANITIZE:-0}" == 1 ]]; then
    FLAGS+=(-O1 -g -fsanitize=thread -fno-omit-frame-pointer)
fi
SOURCES=(firmware/sample/*.cpp firmware/gm/*.cpp firmware/sf2/*.cpp firmware/voice/*.cpp firmware/realtime/*.cpp)
"${CXX:-g++}" "${FLAGS[@]}" "${SOURCES[@]}" tests/app/test_loop_realtime.cpp -lcrypto -o "$OUT/native"
"$OUT/native" assets/sf2/subsets/FM_GM_Open_subset_6MiB.sf2
"${CXX:-g++}" "${FLAGS[@]}" "${SOURCES[@]}" tests/app/test_loop_output.cpp -lcrypto -o "$OUT/output"
# WAV files are deterministic host renders with explicit candidate latency;
# no device execution or human listening approval is inferred from generation.
"$OUT/output" assets/sf2/subsets/FM_GM_Open_subset_6MiB.sf2 "$OUT/audition"
