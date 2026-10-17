#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "${ROOT_DIR}"

CXX="${CXX:-g++}"
BUILD_DIR="${GM128_BUILD_DIR:-build/gm128-full-bank}"
mkdir -p "${BUILD_DIR}"

COMMON_FLAGS=(
  -std=c++17
  -O2
  -Wall
  -Wextra
  -Werror
  -pedantic
  -Ifirmware
)

RUNTIME_SOURCES=(
  firmware/sample/SampleResidencyManager.cpp
  firmware/sample/SampleResidencySourceLoad.cpp
  firmware/sample/SampleResidencyView.cpp
  firmware/sample/PublishedSampleDirectory.cpp
  firmware/sample/ProgramSnapshot.cpp
  firmware/sample/ProgramPrepareController.cpp
  firmware/voice/PitchMath.cpp
  firmware/voice/VoiceRenderer.cpp
  firmware/voice/VoiceRuntimeControl.cpp
  firmware/sf2/Sf2SampleSource.cpp
  firmware/sf2/Sf2PdtaReader.cpp
  firmware/sf2/Sf2ResolvedZone.cpp
  firmware/sf2/Sf2ModulationMath.cpp
  firmware/sf2/Sf2FilterMath.cpp
  firmware/sf2/Sf2ZoneModulatorResolver.cpp
  firmware/sf2/Sf2PlaybackMaterializer.cpp
  firmware/sf2/Sf2PlaybackProgramPreparer.cpp
  firmware/gm/ProgramChangeMailbox.cpp
  firmware/gm/RealtimeProgramChangeHandoff.cpp
  firmware/gm/RealtimeProgramChangeRouter.cpp
  firmware/gm/ProgramChangeControlService.cpp
  firmware/gm/ProgramRuntimeAudioBlock.cpp
  firmware/gm/MidiChannelState.cpp
  firmware/gm/PlaybackProgramSnapshot.cpp
  firmware/gm/ZoneResolver.cpp
)

build_and_run() {
  local output="$1"
  shift
  "${CXX}" "${COMMON_FLAGS[@]}" "$@" -o "${BUILD_DIR}/${output}"
  "${BUILD_DIR}/${output}"
}

printf 'GM128 full-bank qualification\n'
printf '  compiler: %s\n' "${CXX}"
printf '  output:   %s\n' "${BUILD_DIR}"

printf '\n[1/5] Build and run full-bank structure / StrictQualified report\n'
build_and_run test_full_gm128_qualification \
  firmware/sf2/Sf2PdtaReader.cpp \
  firmware/sf2/Sf2ResolvedZone.cpp \
  firmware/sf2/Sf2ZoneModulatorResolver.cpp \
  firmware/voice/PitchMath.cpp \
  firmware/sf2/Sf2PlaybackMaterializer.cpp \
  firmware/sf2/Sf2PlaybackProgramPreparer.cpp \
  firmware/sf2/Sf2SampleSource.cpp \
  tests/gm/test_full_gm128_qualification.cpp

printf '\n[2/5] Build and run full-bank runtime-control transactions\n'
build_and_run test_full_gm128_runtime -pthread \
  "${RUNTIME_SOURCES[@]}" \
  tests/gm/test_program_change_control_service.cpp

printf '\n[3/5] Build and run full-bank 4 MiB / 16-channel stress qualification\n'
build_and_run test_full_gm128_stress -pthread \
  "${RUNTIME_SOURCES[@]}" \
  tests/gm/test_full_gm128_stress.cpp

printf '\n[4/5] Build and run 16-channel Active+READY capacity curve\n'
build_and_run test_full_gm128_capacity -pthread \
  "${RUNTIME_SOURCES[@]}" \
  tests/gm/test_full_gm128_capacity.cpp

printf '\n[5/5] Build and run discovery-driven 32-second host soak\n'
build_and_run test_full_gm128_soak -pthread \
  -include limits \
  "${RUNTIME_SOURCES[@]}" \
  tests/gm/test_full_gm128_soak.cpp

printf '\nGM128 full-bank qualification passed.\n'
