#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"
BASE=6d954a7bc200cd0ec300873e7e106fff2a87a692
OUT="${V1_VOICE_BUILD_DIR:-build/v1-loop-voice}"
mkdir -p "$OUT";OUT="$(cd "$OUT" && pwd)"
FROZEN="$OUT/frozen"
mkdir -p "$FROZEN"
git archive "$BASE" firmware | tar -x -C "$FROZEN"
FLAGS=(-std=c++17 -O2 -pthread -Ifirmware)
SOURCES=(firmware/sample/PublishedSampleDirectory.cpp firmware/voice/VoiceRenderer.cpp firmware/voice/VoiceEffectsBus.cpp firmware/voice/VoiceRuntimeControl.cpp)
for name in VoiceRenderer VoiceEffectsBus VoiceRuntimeControl; do
  (cd "$FROZEN"; "${CXX:-g++}" "${FLAGS[@]}" -c "firmware/voice/$name.cpp" -o "$OUT/base-$name.o")
  "${CXX:-g++}" "${FLAGS[@]}" -c "firmware/voice/$name.cpp" -o "$OUT/default-$name.o"
  cmp "$OUT/base-$name.o" "$OUT/default-$name.o"
done
(cd "$FROZEN"; "${CXX:-g++}" "${FLAGS[@]}" "${SOURCES[@]}" "$ROOT/tests/voice/test_loop_voice_disabled_trace.cpp" -o "$OUT/trace-base")
"$OUT/trace-base" > "$OUT/trace-base.txt"
for flag in 0 1; do
  "${CXX:-g++}" "${FLAGS[@]}" -DGM_SYNTH_EXPERIMENTAL_VOICE_LOOP=$flag "${SOURCES[@]}" tests/voice/test_loop_voice_disabled_trace.cpp -o "$OUT/trace-$flag"
  "$OUT/trace-$flag" > "$OUT/trace-$flag.txt"
  cmp "$OUT/trace-base.txt" "$OUT/trace-$flag.txt"
done
cat > "$OUT/size.cpp" <<'CPP'
#include "voice/VoiceRenderer.h"
#include <iostream>
int main(){using namespace gm_synth::voice;std::cout<<sizeof(Voice)<<' '<<sizeof(VoiceStartParams)<<' '<<sizeof(VoicePool)<<'\n';}
CPP
(cd "$FROZEN"; "${CXX:-g++}" "${FLAGS[@]}" "$OUT/size.cpp" -o "$OUT/size-base")
"$OUT/size-base" > "$OUT/size-base.txt"
for flag in 0 1; do
  "${CXX:-g++}" "${FLAGS[@]}" -DGM_SYNTH_EXPERIMENTAL_VOICE_LOOP=$flag "$OUT/size.cpp" -o "$OUT/size-$flag"
  "$OUT/size-$flag" > "$OUT/size-$flag.txt"
done
cmp "$OUT/size-base.txt" "$OUT/size-0.txt"
echo 'Default-off native objects and layouts: byte-exact PASS'
cat "$OUT/trace-base.txt"
echo "Host-only sizes Voice/Params/Pool: off=$(cat "$OUT/size-0.txt") enabled=$(cat "$OUT/size-1.txt")"
