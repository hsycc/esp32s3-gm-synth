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
ID_PROBE="${BUILD_DIR}/test_sf2_id_space"

"${CXX}" "${COMMON_FLAGS[@]}" \
  firmware/sf2/Sf2PdtaReader.cpp \
  tests/gm/test_sf2_id_space.cpp \
  -o "${ID_PROBE}"

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

probe_asset() {
  local path="$1"
  local asset="$2"
  "${ID_PROBE}" "${path}" "${asset}" | tee "${BUILD_DIR}/${asset}.id-space.log"
}

run_asset() {
  local path="$1"
  local asset="$2"
  local profile="$3"
  local required="$4"
  local source_kind="$5"
  local log="${BUILD_DIR}/${asset}.log"
  local raw_log="${BUILD_DIR}/${asset}.raw.log"

  printf '\n[M7] asset=%s source=%s path=%s profile=%s required_accept=%s\n' \
    "${asset}" "${source_kind}" "${path}" "${profile}" "${required}"

  probe_asset "${path}" "${asset}"

  set +e
  "${SCANNER}" "${path}" "${asset}" "${profile}" "${required}" "${source_kind}" \
    2>&1 | tee "${raw_log}"
  local scanner_status=${PIPESTATUS[0]}
  set -e

  if [[ "${scanner_status}" -eq 0 ]]; then
    cp "${raw_log}" "${log}"
    return 0
  fi

  # Sf2SampleSource error 6 is TooManySamples. The current runtime keeps a
  # bounded 512-entry sample metadata table, so an otherwise parseable user
  # SoundFont that exceeds this limit is an explicit compatibility rejection,
  # not a qualification-runner failure. Preserve the raw scanner log and write
  # a normalized asset-level classification for the portfolio reporter.
  if [[ "${required}" == "0" ]] && \
     grep -Fq "M7_SF2_FATAL asset=${asset} reason=sample_source_open error=6" "${raw_log}"; then
    local file_bytes
    file_bytes="$(wc -c < "${path}" | tr -d ' ')"
    printf '%s\n' \
      "M7_SF2_ASSET_REJECTED asset=${asset} profile=${profile} required_accept=${required} source_kind=${source_kind} path=${path} reason=runtime_sample_table_limit file_bytes=${file_bytes} sample_source_error=6 runtime_limit=512" \
      | tee "${log}"
    return 0
  fi

  cp "${raw_log}" "${log}"
  return "${scanner_status}"
}

FM_PATH="assets/sf2/full/FM_GM_SoundFont_v0_2_1_mini.sf2"
GENERALUSER_FULL_PATH="assets/sf2/full/GeneralUser-GS.sf2"
GENERALUSER_SUBSET_PATH="assets/sf2/subsets/GeneralUser_GS_subset_6MiB.sf2"
FLUID_FULL_PATH="assets/sf2/full/FluidR3_GM.sf2"
FLUID_SUBSET_PATH="assets/sf2/subsets/FluidR3_GM_subset_6MiB.sf2"

if ! is_real_sf2 "${FM_PATH}"; then
  echo "required tracked FM/GM asset is unavailable or not an SF2: ${FM_PATH}" >&2
  exit 1
fi

GENERALUSER_PATH="${GENERALUSER_SUBSET_PATH}"
GENERALUSER_SOURCE_KIND="subset"
if is_real_sf2 "${GENERALUSER_FULL_PATH}"; then
  GENERALUSER_PATH="${GENERALUSER_FULL_PATH}"
  GENERALUSER_SOURCE_KIND="full"
elif ! is_real_sf2 "${GENERALUSER_SUBSET_PATH}"; then
  echo "neither full nor subset GeneralUser GS asset is available" >&2
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
run_asset "${GENERALUSER_PATH}" "generaluser_gs" "native" 0 "${GENERALUSER_SOURCE_KIND}"
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
