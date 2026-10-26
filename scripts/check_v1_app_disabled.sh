#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"
BASE=7a019caf209d07f500fdb81b22c0b4ff4d201f76
OUT="${V1_APP_BUILD_DIR:-build/v1-loop-app}/disabled"
mkdir -p "$OUT/frozen";OUT="$(cd "$OUT" && pwd)"
git archive "$BASE" firmware | tar -x -C "$OUT/frozen"
FLAGS=(-std=c++17 -O2 -pthread -Ifirmware)
for name in ZoneResolver RealtimeProgramChangeRouter; do
  (cd "$OUT/frozen"; "${CXX:-g++}" "${FLAGS[@]}" -c "firmware/gm/$name.cpp" -o "$OUT/old-$name.o")
  "${CXX:-g++}" "${FLAGS[@]}" -c "firmware/gm/$name.cpp" -o "$OUT/new-$name.o"
  cmp "$OUT/old-$name.o" "$OUT/new-$name.o"
done
SOURCES=(firmware/sample/*.cpp firmware/gm/*.cpp firmware/sf2/*.cpp firmware/voice/*.cpp firmware/realtime/*.cpp)
(cd "$OUT/frozen"; "${CXX:-g++}" "${FLAGS[@]}" "${SOURCES[@]}" "$ROOT/tests/app/test_loop_app_no_plan.cpp" -o "$OUT/old")
"$OUT/old" > "$OUT/old.txt"
for flag in 0 1; do
 "${CXX:-g++}" "${FLAGS[@]}" -DGM_SYNTH_EXPERIMENTAL_VOICE_LOOP=$flag "${SOURCES[@]}" tests/app/test_loop_app_no_plan.cpp -o "$OUT/new-$flag"
 "$OUT/new-$flag" > "$OUT/new-$flag.txt"
 cmp "$OUT/old.txt" "$OUT/new-$flag.txt"
done
cat "$OUT/old.txt"
echo 'Default-off GM objects and old/off/on-without-hook traces: exact PASS'
