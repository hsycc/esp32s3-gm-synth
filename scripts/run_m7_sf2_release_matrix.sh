#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "${ROOT_DIR}"

CXX="${CXX:-g++}"
BUILD_DIR="${M7_SF2_BUILD_DIR:-build/m7-sf2-release}"
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

SCANNER="${BUILD_DIR}/test_sf2_release_matrix"
"${CXX}" "${COMMON_FLAGS[@]}" \
  firmware/sf2/Sf2PdtaReader.cpp \
  firmware/sf2/Sf2ResolvedZone.cpp \
  firmware/sf2/Sf2ZoneModulatorResolver.cpp \
  firmware/voice/PitchMath.cpp \
  firmware/sf2/Sf2PlaybackMaterializer.cpp \
  firmware/sf2/Sf2PlaybackProgramPreparer.cpp \
  firmware/sf2/Sf2SampleSource.cpp \
  tests/gm/test_sf2_release_matrix.cpp \
  -o "${SCANNER}"

is_real_sf2() {
  local path="$1"
  [[ -s "${path}" ]] || return 1
  local magic
  magic="$(dd if="${path}" bs=1 count=4 2>/dev/null || true)"
  [[ "${magic}" == "RIFF" ]]
}

run_asset() {
  local path="$1"
  local asset="$2"
  local profile="$3"
  local required="$4"
  local source_kind="$5"
  local log="${BUILD_DIR}/${asset}.log"

  printf '\n[M7] asset=%s source=%s path=%s profile=%s required_accept=%s\n' \
    "${asset}" "${source_kind}" "${path}" "${profile}" "${required}"
  "${SCANNER}" "${path}" "${asset}" "${profile}" "${required}" "${source_kind}" \
    2>&1 | tee "${log}"
}

FM_PATH="assets/sf2/full/FM_GM_SoundFont_v0_2_1_mini.sf2"
GENERALUSER_PATH="assets/sf2/full/GeneralUser-GS.sf2"
FLUID_FULL_PATH="assets/sf2/full/FluidR3_GM.sf2"
FLUID_SUBSET_PATH="assets/sf2/subsets/FluidR3_GM_subset_6MiB.sf2"

if ! is_real_sf2 "${FM_PATH}"; then
  echo "required tracked FM/GM asset is unavailable or not an SF2: ${FM_PATH}" >&2
  exit 1
fi
if ! is_real_sf2 "${GENERALUSER_PATH}"; then
  echo "GeneralUser GS asset is unavailable or not an SF2: ${GENERALUSER_PATH}" >&2
  exit 1
fi

FLUID_PATH="${FLUID_SUBSET_PATH}"
FLUID_SOURCE_KIND="subset"
if is_real_sf2 "${FLUID_FULL_PATH}"; then
  FLUID_PATH="${FLUID_FULL_PATH}"
  FLUID_SOURCE_KIND="full"
elif ! is_real_sf2 "${FLUID_SUBSET_PATH}"; then
  echo "neither full nor subset FluidR3 asset is available" >&2
  exit 1
fi

run_asset "${FM_PATH}" "fm_gm_open" "gm128" 1 "full"
run_asset "${GENERALUSER_PATH}" "generaluser_gs" "native" 0 "full"
run_asset "${FLUID_PATH}" "fluidr3_gm" "native" 0 "${FLUID_SOURCE_KIND}"

python -m tools.sf2_release.report \
  --log "${BUILD_DIR}/fm_gm_open.log" \
  --log "${BUILD_DIR}/generaluser_gs.log" \
  --log "${BUILD_DIR}/fluidr3_gm.log" \
  --json "${BUILD_DIR}/sf2-release-portfolio.json" \
  --markdown "${BUILD_DIR}/sf2-release-portfolio.md" \
  --min-assets 3 \
  --title "M7 representative native-SF2 release portfolio"

printf '\nM7 representative native-SF2 classification completed.\n'
