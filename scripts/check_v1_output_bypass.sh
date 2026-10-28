#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"
BASE=4b097e8bd65e86893e343943342b51e42377c03d
OUT="${V1_OUTPUT_BYPASS_DIR:-build/v1-output-bypass}"
mkdir -p "$OUT/frozen"
OUT="$(cd "$OUT" && pwd)"
git archive "$BASE" firmware | tar -x -C "$OUT/frozen"
FLAGS=(-std=c++17 -O2 -pthread -Ifirmware)
(cd "$OUT/frozen"; "${CXX:-g++}" "${FLAGS[@]}" -c firmware/realtime/ScheduledAudioBlockRenderer.cpp -o "$OUT/old.o")
"${CXX:-g++}" "${FLAGS[@]}" -c firmware/realtime/ScheduledAudioBlockRenderer.cpp -o "$OUT/current.o"
cmp "$OUT/old.o" "$OUT/current.o"
SOURCES=(firmware/sample/*.cpp firmware/gm/*.cpp firmware/sf2/*.cpp firmware/voice/*.cpp firmware/realtime/*.cpp)
(cd "$OUT/frozen"; "${CXX:-g++}" "${FLAGS[@]}" "${SOURCES[@]}" "$ROOT/tests/realtime/test_output_bypass_trace.cpp" -o "$OUT/old")
"$OUT/old" > "$OUT/old.txt"
for flag in 0 1; do
    "${CXX:-g++}" "${FLAGS[@]}" -DGM_SYNTH_EXPERIMENTAL_VOICE_LOOP=$flag "${SOURCES[@]}" tests/realtime/test_output_bypass_trace.cpp -o "$OUT/current-$flag"
    "$OUT/current-$flag" > "$OUT/current-$flag.txt"
    cmp "$OUT/old.txt" "$OUT/current-$flag.txt"
done
cat "$OUT/old.txt"
echo 'Default-off renderer object and complete no-hook PCM/counters: exact PASS'
