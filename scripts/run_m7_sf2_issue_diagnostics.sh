#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "${ROOT_DIR}"

CXX="${CXX:-g++}"
BUILD_DIR="${M7_SF2_BUILD_DIR:-build/m7-sf2-issues}"
mkdir -p "${BUILD_DIR}"

DIAG="${BUILD_DIR}/test_sf2_issue_diagnostics"
"${CXX}" -std=c++17 -O2 -Wall -Wextra -Werror -pedantic -Ifirmware \
  firmware/sf2/Sf2PdtaReader.cpp \
  firmware/sf2/Sf2ResolvedZone.cpp \
  firmware/sf2/Sf2ZoneModulatorResolver.cpp \
  firmware/voice/PitchMath.cpp \
  firmware/sf2/Sf2PlaybackMaterializer.cpp \
  firmware/sf2/Sf2PlaybackProgramPreparer.cpp \
  tests/gm/test_sf2_issue_diagnostics.cpp \
  -o "${DIAG}"

is_real_sf2() {
  local path="$1"
  [[ -s "${path}" ]] || return 1
  [[ "$(dd if="${path}" bs=1 count=4 2>/dev/null || true)" == "RIFF" ]]
}

pick_asset() {
  local full="$1"
  local subset="$2"
  if is_real_sf2 "${full}"; then
    printf '%s' "${full}"
  else
    printf '%s' "${subset}"
  fi
}

run_one() {
  local path="$1"
  local asset="$2"
  if ! is_real_sf2 "${path}"; then
    echo "M7_ISSUE_FATAL asset=${asset} reason=asset_unavailable path=${path}" >&2
    return 1
  fi
  "${DIAG}" "${path}" "${asset}" | tee "${BUILD_DIR}/${asset}.issues.log"
}

FM_PATH="assets/sf2/full/FM_GM_SoundFont_v0_2_1_mini.sf2"
GENERALUSER_PATH="$(pick_asset assets/sf2/full/GeneralUser-GS.sf2 assets/sf2/subsets/GeneralUser_GS_subset_6MiB.sf2)"
FLUID_PATH="$(pick_asset assets/sf2/full/FluidR3_GM.sf2 assets/sf2/subsets/FluidR3_GM_subset_6MiB.sf2)"

run_one "${FM_PATH}" fm_gm_open
run_one "${GENERALUSER_PATH}" generaluser_gs
run_one "${FLUID_PATH}" fluidr3_gm

{
  echo "# M7 SF2 issue diagnostics"
  echo
  for asset in fm_gm_open generaluser_gs fluidr3_gm; do
    grep '^M7_ISSUE_SUMMARY' "${BUILD_DIR}/${asset}.issues.log" || true
    grep '^M7_ISSUE_COUNT' "${BUILD_DIR}/${asset}.issues.log" | grep -v 'rejected_programs=0$' || true
    echo
  done
} > "${BUILD_DIR}/issue-summary.txt"

cat "${BUILD_DIR}/issue-summary.txt"
