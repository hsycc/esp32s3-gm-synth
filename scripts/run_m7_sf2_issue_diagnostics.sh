#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "${ROOT_DIR}"

CXX="${CXX:-g++}"
BUILD_DIR="${M7_SF2_BUILD_DIR:-build/m7-sf2-issues}"
mkdir -p "${BUILD_DIR}"

python3 -m unittest tests.test_m7_modulator_closure -v

COMMON_SF2_SOURCES=(
  firmware/sf2/Sf2PdtaReader.cpp
  firmware/sf2/Sf2ResolvedZone.cpp
  firmware/sf2/Sf2ZoneModulatorResolver.cpp
  firmware/voice/PitchMath.cpp
  firmware/sf2/Sf2PlaybackMaterializer.cpp
  firmware/sf2/Sf2PlaybackProgramPreparer.cpp
)

COMPAT="${BUILD_DIR}/test_sf2_unlinked_stereo_compat"
"${CXX}" -std=c++17 -O2 -Wall -Wextra -Werror -pedantic -Ifirmware \
  "${COMMON_SF2_SOURCES[@]}" \
  tests/sf2/test_sf2_unlinked_stereo_compat.cpp \
  -o "${COMPAT}"
"${COMPAT}"

GAIN="${BUILD_DIR}/test_sf2_gain_clamp_qualification"
"${CXX}" -std=c++17 -O2 -Wall -Wextra -Werror -pedantic -Ifirmware \
  firmware/voice/PitchMath.cpp \
  firmware/sf2/Sf2PlaybackMaterializer.cpp \
  tests/sf2/test_sf2_gain_clamp_qualification.cpp \
  -o "${GAIN}"
"${GAIN}"

NOTE_ON_MOD="${BUILD_DIR}/test_sf2_note_on_modulator"
"${CXX}" -std=c++17 -O2 -Wall -Wextra -Werror -pedantic -Ifirmware \
  firmware/sf2/Sf2ModulationMath.cpp \
  tests/sf2/test_sf2_note_on_modulator.cpp \
  -o "${NOTE_ON_MOD}"
"${NOTE_ON_MOD}"

NOTE_ON_SET="${BUILD_DIR}/test_sf2_note_on_modulator_set"
"${CXX}" -std=c++17 -O2 -Wall -Wextra -Werror -pedantic -Ifirmware \
  tests/sf2/test_sf2_note_on_modulator_set.cpp \
  -o "${NOTE_ON_SET}"
"${NOTE_ON_SET}"

NOTE_ON_TARGETS="${BUILD_DIR}/test_sf2_note_on_modulator_targets"
"${CXX}" -std=c++17 -O2 -Wall -Wextra -Werror -pedantic -Ifirmware \
  firmware/sf2/Sf2ModulationMath.cpp \
  tests/sf2/test_sf2_note_on_modulator_targets.cpp \
  -o "${NOTE_ON_TARGETS}"
"${NOTE_ON_TARGETS}"

KEY_INDEX="${BUILD_DIR}/test_playback_program_key_index"
"${CXX}" -std=c++17 -O2 -Wall -Wextra -Werror -pedantic -Ifirmware \
  tests/gm/test_playback_program_key_index.cpp \
  -o "${KEY_INDEX}"
"${KEY_INDEX}"

DIAG="${BUILD_DIR}/test_sf2_issue_diagnostics"
"${CXX}" -std=c++17 -O2 -Wall -Wextra -Werror -pedantic -Ifirmware \
  "${COMMON_SF2_SOURCES[@]}" \
  tests/gm/test_sf2_issue_diagnostics.cpp \
  -o "${DIAG}"

MOD_INVENTORY="${BUILD_DIR}/test_sf2_modulator_inventory"
"${CXX}" -std=c++17 -O2 -Wall -Wextra -Werror -pedantic -Ifirmware \
  firmware/sf2/Sf2PdtaReader.cpp \
  tests/gm/test_sf2_modulator_inventory.cpp \
  -o "${MOD_INVENTORY}"

ZONE_SHAPE="${BUILD_DIR}/test_sf2_zone_shape_inventory"
"${CXX}" -std=c++17 -O2 -Wall -Wextra -Werror -pedantic -Ifirmware \
  firmware/sf2/Sf2PdtaReader.cpp \
  firmware/sf2/Sf2ResolvedZone.cpp \
  tests/gm/test_sf2_zone_shape_inventory.cpp \
  -o "${ZONE_SHAPE}"

PREPARED_ROUTE_INVENTORY="${BUILD_DIR}/test_sf2_prepared_note_on_route_inventory"
"${CXX}" -std=c++17 -O2 -Wall -Wextra -Werror -pedantic -Ifirmware \
  firmware/sf2/Sf2PdtaReader.cpp \
  firmware/sf2/Sf2ResolvedZone.cpp \
  firmware/sf2/Sf2PreparedNoteOnRouteResolver.cpp \
  tests/gm/test_sf2_prepared_note_on_route_inventory.cpp \
  -o "${PREPARED_ROUTE_INVENTORY}"

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
  "${MOD_INVENTORY}" "${path}" "${asset}" | tee "${BUILD_DIR}/${asset}.modulators.log"
  "${ZONE_SHAPE}" "${path}" "${asset}" | tee "${BUILD_DIR}/${asset}.zone-shape.log"
  "${PREPARED_ROUTE_INVENTORY}" "${path}" "${asset}" | \
    tee "${BUILD_DIR}/${asset}.prepared-routes.log"
  python3 tools/m7_release/modulator_closure.py \
    "${BUILD_DIR}/${asset}.modulators.log" \
    --asset "${asset}" | tee "${BUILD_DIR}/${asset}.modulator-closure.log"
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
    grep '^M7_MOD_SUMMARY' "${BUILD_DIR}/${asset}.modulators.log" || true
    grep '^M7_MOD_CLOSURE_SUMMARY' "${BUILD_DIR}/${asset}.modulator-closure.log" || true
    grep '^M7_MOD_CLOSURE_CATEGORY' "${BUILD_DIR}/${asset}.modulator-closure.log" || true
    grep '^M7_PREPARED_ROUTE_SUMMARY' "${BUILD_DIR}/${asset}.prepared-routes.log" || true
    grep '^M7_ZONE_SHAPE_SUMMARY' "${BUILD_DIR}/${asset}.zone-shape.log" || true
    echo
  done
} > "${BUILD_DIR}/issue-summary.txt"

cat "${BUILD_DIR}/issue-summary.txt"
