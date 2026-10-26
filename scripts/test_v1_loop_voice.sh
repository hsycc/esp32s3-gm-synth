#!/usr/bin/env bash
set -euo pipefail
cd "$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
OUT="${V1_VOICE_BUILD_DIR:-build/v1-loop-voice}"
mkdir -p "$OUT"
FLAGS=(-std=c++17 -O2 -Wall -Wextra -Werror -pedantic -pthread -Ifirmware -DGM_SYNTH_EXPERIMENTAL_VOICE_LOOP=1)
if [[ "${SANITIZE:-0}" == 1 ]]; then FLAGS+=(-O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer); fi
SOURCES=(firmware/sample/PublishedSampleDirectory.cpp firmware/voice/VoiceRenderer.cpp firmware/voice/VoiceEffectsBus.cpp firmware/voice/VoiceRuntimeControl.cpp firmware/voice/GlobalEffectsProcessor.cpp)
"${CXX:-g++}" "${FLAGS[@]}" "${SOURCES[@]}" tests/voice/test_loop_voice_integration.cpp -o "$OUT/test_voice_integration"
"$OUT/test_voice_integration"
for name in test_voice_realtime_fast_path test_voice_release_realtime_fast_path test_effects_send_scaling; do
  "${CXX:-g++}" "${FLAGS[@]}" "${SOURCES[@]}" "tests/voice/$name.cpp" -o "$OUT/$name"
  "$OUT/$name"
done

"${CXX:-g++}" "${FLAGS[@]}" "${SOURCES[@]}" \
 firmware/sample/SampleResidencyManager.cpp firmware/sample/SampleResidencySourceLoad.cpp \
 firmware/sample/SampleResidencyView.cpp firmware/sample/ProgramSnapshot.cpp firmware/sample/ProgramPrepareController.cpp \
 firmware/gm/PlaybackProgramSnapshot.cpp firmware/sf2/Sf2SampleSource.cpp firmware/sf2/Sf2PdtaReader.cpp \
 firmware/sf2/Sf2ResolvedZone.cpp firmware/sf2/Sf2ZoneModulatorResolver.cpp firmware/voice/PitchMath.cpp \
 firmware/sf2/Sf2PlaybackMaterializer.cpp firmware/sf2/Sf2PlaybackProgramPreparer.cpp \
 tools/loop_voice_probe.cpp -o "$OUT/loop_voice_probe"
