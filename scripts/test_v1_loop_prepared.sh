#!/usr/bin/env bash
set -euo pipefail
cd "$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
OUT="${V1_PREPARED_BUILD_DIR:-build/v1-loop-prepared}"
mkdir -p "$OUT"
FLAGS=(-std=c++17 -O2 -Wall -Wextra -Werror -pedantic -pthread -Ifirmware)
if [[ "${SANITIZE:-0}" == 1 ]]; then FLAGS+=(-O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer); fi
COMMON=(firmware/sample/SampleResidencyManager.cpp firmware/sample/SampleResidencySourceLoad.cpp
 firmware/sample/SampleResidencyView.cpp
 firmware/sample/PublishedSampleDirectory.cpp firmware/sample/ProgramSnapshot.cpp
 firmware/sample/ProgramPrepareController.cpp firmware/gm/PlaybackProgramSnapshot.cpp)
"${CXX:-g++}" "${FLAGS[@]}" "${COMMON[@]}" tests/voice/test_loop_crossfade_prepared.cpp -o "$OUT/test_prepared"
"$OUT/test_prepared"
"${CXX:-g++}" "${FLAGS[@]}" "${COMMON[@]}" firmware/sf2/Sf2SampleSource.cpp \
 firmware/sf2/Sf2PdtaReader.cpp firmware/sf2/Sf2ResolvedZone.cpp firmware/sf2/Sf2ZoneModulatorResolver.cpp \
 firmware/voice/PitchMath.cpp firmware/sf2/Sf2PlaybackMaterializer.cpp firmware/sf2/Sf2PlaybackProgramPreparer.cpp \
 tools/loop_prepared_probe.cpp -o "$OUT/loop_prepared_probe"
