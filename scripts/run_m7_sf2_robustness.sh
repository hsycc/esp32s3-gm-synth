#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "${ROOT_DIR}"

BUILD_DIR="${M7_SF2_BUILD_DIR:-build/m7-sf2-release}"
SCANNER="${BUILD_DIR}/test_sf2_release_matrix"
SOURCE="assets/sf2/subsets/FluidR3_GM_subset_6MiB.sf2"
CASE_DIR="${BUILD_DIR}/robustness"
mkdir -p "${CASE_DIR}"

if [[ ! -x "${SCANNER}" ]]; then
  echo "M7 release scanner has not been built: ${SCANNER}" >&2
  exit 1
fi
if [[ ! -s "${SOURCE}" ]] || [[ "$(dd if="${SOURCE}" bs=1 count=4 2>/dev/null || true)" != "RIFF" ]]; then
  echo "M7 robustness source fixture is unavailable: ${SOURCE}" >&2
  exit 1
fi

python3 - "${SOURCE}" "${CASE_DIR}" <<'PY'
from pathlib import Path
import sys

source = Path(sys.argv[1]).read_bytes()
out = Path(sys.argv[2])
out.mkdir(parents=True, exist_ok=True)

(out / "too-short.sf2").write_bytes(source[:8])

bad_form = bytearray(source)
bad_form[8:12] = b"NOPE"
(out / "bad-form.sf2").write_bytes(bad_form)

bad_riff_size = bytearray(source)
bad_riff_size[4:8] = (0xFFFFFFFF).to_bytes(4, "little")
(out / "bad-riff-size.sf2").write_bytes(bad_riff_size)

(out / "truncated-payload.sf2").write_bytes(source[: max(12, len(source) // 2)])
PY

explicit_rejections=0
crashes=0

expect_rejection() {
  local case_name="$1"
  local path="$2"
  local expected="$3"
  local log="${CASE_DIR}/${case_name}.log"

  set +e
  "${SCANNER}" "${path}" "malformed_${case_name}" native 0 synthetic >"${log}" 2>&1
  local status=$?
  set -e

  if [[ "${status}" -ge 128 ]]; then
    crashes=$((crashes + 1))
    echo "M7_SF2_ROBUSTNESS case=${case_name} status=CRASH scanner_exit=${status}"
    cat "${log}"
    return 1
  fi
  if [[ "${status}" -eq 0 ]]; then
    echo "M7_SF2_ROBUSTNESS case=${case_name} status=UNEXPECTED_ACCEPT scanner_exit=0"
    cat "${log}"
    return 1
  fi
  if ! grep -Fq "${expected}" "${log}"; then
    echo "M7_SF2_ROBUSTNESS case=${case_name} status=WRONG_REJECTION scanner_exit=${status} expected=${expected}"
    cat "${log}"
    return 1
  fi

  explicit_rejections=$((explicit_rejections + 1))
  echo "M7_SF2_ROBUSTNESS case=${case_name} status=REJECTED scanner_exit=${status} reason=${expected}"
}

expect_rejection "too_short" "${CASE_DIR}/too-short.sf2" \
  "M7_SF2_FATAL asset=malformed_too_short reason=pdta_open error=1"
expect_rejection "bad_form" "${CASE_DIR}/bad-form.sf2" \
  "M7_SF2_FATAL asset=malformed_bad_form reason=pdta_open error=2"
expect_rejection "bad_riff_size" "${CASE_DIR}/bad-riff-size.sf2" \
  "M7_SF2_FATAL asset=malformed_bad_riff_size reason=pdta_open error=2"
expect_rejection "truncated_payload" "${CASE_DIR}/truncated-payload.sf2" \
  "M7_SF2_FATAL asset=malformed_truncated_payload reason=pdta_open error=2"

if [[ "${explicit_rejections}" -ne 4 ]] || [[ "${crashes}" -ne 0 ]]; then
  echo "M7_SF2_ROBUSTNESS_SUMMARY cases=4 explicit_rejections=${explicit_rejections} crashes=${crashes} result=FAIL"
  exit 1
fi

echo "M7_SF2_ROBUSTNESS_SUMMARY cases=4 explicit_rejections=${explicit_rejections} crashes=${crashes} result=PASS"
