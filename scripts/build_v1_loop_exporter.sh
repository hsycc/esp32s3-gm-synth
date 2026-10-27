#!/usr/bin/env bash
set -euo pipefail
cd "$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${V1_LOOP_BUILD_DIR:-build/v1-loop-host}"
mkdir -p "$BUILD_DIR"
"${CXX:-c++}" -std=c++17 -O2 -Wall -Wextra -Werror -pedantic -Ifirmware \
  firmware/sf2/Sf2PdtaReader.cpp \
  firmware/sf2/Sf2ResolvedZone.cpp \
  firmware/sf2/Sf2ZoneModulatorResolver.cpp \
  firmware/voice/PitchMath.cpp \
  firmware/sf2/Sf2PlaybackMaterializer.cpp \
  firmware/sf2/Sf2PlaybackProgramPreparer.cpp \
  tools/export_prepared_loops.cpp -o "$BUILD_DIR/export_prepared_loops"
echo "Built Host exporter: $BUILD_DIR/export_prepared_loops"
