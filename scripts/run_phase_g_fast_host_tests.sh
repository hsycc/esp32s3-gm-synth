#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "${ROOT_DIR}"

# Keep the existing consolidated runner as the canonical Phase G build matrix,
# but remove the two tracked-full-bank calls from its test entry points for this
# fast CI lane. Source files are restored even when a local invocation fails.
PREPARER_TEST="tests/sf2/test_sf2_playback_program_preparer.cpp"
CONTROL_TEST="tests/gm/test_program_change_control_service.cpp"
TMP_DIR="$(mktemp -d)"
cp "${PREPARER_TEST}" "${TMP_DIR}/preparer.cpp"
cp "${CONTROL_TEST}" "${TMP_DIR}/control.cpp"
restore_sources() {
  cp "${TMP_DIR}/preparer.cpp" "${PREPARER_TEST}"
  cp "${TMP_DIR}/control.cpp" "${CONTROL_TEST}"
  rm -rf "${TMP_DIR}"
}
trap restore_sources EXIT

python3 - <<'PY'
from pathlib import Path

removals = {
    Path("tests/sf2/test_sf2_playback_program_preparer.cpp"): [
        "    testFullFmGm128MelodicProgramsStrictlyPrepare();\n",
        "    testFullFmStandardDrumKitStrictlyPreparesAndCoversGmKeys();\n",
    ],
    Path("tests/gm/test_program_change_control_service.cpp"): [
        "    testFullFmProgramChangeResidencyCommitAndNoteOn();\n",
        "    testFullFmGmResetCommitsAll16ChannelsAndStartsVoices();\n",
    ],
}

for path, calls in removals.items():
    text = path.read_text(encoding="utf-8")
    for call in calls:
        if text.count(call) != 1:
            raise SystemExit(f"{path}: expected exactly one full-bank call: {call.strip()}")
        text = text.replace(call, "")
    path.write_text(text, encoding="utf-8")
PY

bash scripts/run_phase_g_host_tests.sh
