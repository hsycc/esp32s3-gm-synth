#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "${ROOT_DIR}"

CXX="${CXX:-g++}"
BUILD_DIR="${PHASE_G_BUILD_DIR:-build/phase-g-host-tests}"
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

printf 'Phase G host validation\n'
printf '  compiler: %s\n' "${CXX}"
printf '  output:   %s\n' "${BUILD_DIR}"

printf '\n[1/9] Validate analyzed realtime zone bounds\n'
python3 - <<'PY'
import glob
import json
import pathlib
import re

playback_header = pathlib.Path("firmware/gm/PlaybackProgramSnapshot.h").read_text()
match = re.search(r"kMaxZones\s*=\s*(\d+)", playback_header)
if not match:
    raise SystemExit("unable to read PlaybackProgramSnapshot::kMaxZones")
zone_limit = int(match.group(1))

voice_header = pathlib.Path("firmware/voice/VoiceRenderer.h").read_text()
match = re.search(r"kHardVoiceCap\s*=\s*(\d+)", voice_header)
if not match:
    raise SystemExit("unable to read VoicePool::kHardVoiceCap")
overlap_limit = int(match.group(1))

worst_zones = None
worst_overlap = None
checked = 0
for path in sorted(glob.glob("assets/sf2/manifests/*.analysis.json")):
    with open(path, "r", encoding="utf-8") as handle:
        analysis = json.load(handle)
    for program in analysis.get("programs", []):
        if "max_overlapping_zones" not in program:
            raise SystemExit(f"{path}: analyzed program is missing max_overlapping_zones")
        count = int(program.get("resolved_zone_count", 0))
        overlap = int(program["max_overlapping_zones"])
        checked += 1
        identity = (
            path,
            int(program.get("bank", 0)),
            int(program.get("program", 0)),
            program.get("name", ""),
        )
        zone_item = (count, *identity)
        overlap_item = (overlap, *identity)
        if worst_zones is None or zone_item[0] > worst_zones[0]:
            worst_zones = zone_item
        if worst_overlap is None or overlap_item[0] > worst_overlap[0]:
            worst_overlap = overlap_item
        if count > zone_limit:
            raise SystemExit(
                f"{path}: bank={identity[1]} program={identity[2]} "
                f"name={identity[3]!r} resolved_zone_count={count} "
                f"exceeds kMaxZones={zone_limit}"
            )
        if overlap > overlap_limit:
            raise SystemExit(
                f"{path}: bank={identity[1]} program={identity[2]} "
                f"name={identity[3]!r} max_overlapping_zones={overlap} "
                f"exceeds VoicePool::kHardVoiceCap={overlap_limit}"
            )

if checked == 0:
    raise SystemExit("no analyzed SF2 programs found")
print(
    f"validated {checked} analyzed programs: kMaxZones={zone_limit}, "
    f"VoicePool::kHardVoiceCap={overlap_limit}"
)
print(
    f"worst resolved zones={worst_zones[0]}: {worst_zones[1]} "
    f"bank={worst_zones[2]} program={worst_zones[3]} name={worst_zones[4]!r}"
)
print(
    f"worst simultaneous overlap={worst_overlap[0]}: {worst_overlap[1]} "
    f"bank={worst_overlap[2]} program={worst_overlap[3]} name={worst_overlap[4]!r}"
)
PY

printf '\n[2/9] Build and run playback-zone runtime tests\n'
"${CXX}" "${COMMON_FLAGS[@]}" -pthread \
  firmware/sample/PublishedSampleDirectory.cpp \
  firmware/sample/ProgramSnapshot.cpp \
  firmware/voice/VoiceRenderer.cpp \
  firmware/sf2/Sf2ModulationMath.cpp \
  firmware/gm/PlaybackProgramSnapshot.cpp \
  firmware/gm/ZoneResolver.cpp \
  firmware/realtime/RealtimeEventQueue.cpp \
  firmware/realtime/BlockEventScheduler.cpp \
  firmware/realtime/ScheduledAudioBlockRenderer.cpp \
  tests/gm/test_playback_zone_runtime.cpp \
  -o "${BUILD_DIR}/test_playback_zone_runtime"
"${BUILD_DIR}/test_playback_zone_runtime"

printf '\n[3/9] Build and run random-access PDTA reader tests\n'
"${CXX}" "${COMMON_FLAGS[@]}" \
  firmware/sf2/Sf2PdtaReader.cpp \
  tests/sf2/test_sf2_pdta_reader.cpp \
  -o "${BUILD_DIR}/test_sf2_pdta_reader"
"${BUILD_DIR}/test_sf2_pdta_reader"

printf '\n[4/9] Build and run native resolved-zone tests\n'
"${CXX}" "${COMMON_FLAGS[@]}" \
  firmware/sf2/Sf2PdtaReader.cpp \
  firmware/sf2/Sf2ResolvedZone.cpp \
  tests/sf2/test_sf2_resolved_zone.cpp \
  -o "${BUILD_DIR}/test_sf2_resolved_zone"
"${BUILD_DIR}/test_sf2_resolved_zone"

printf '\n[5/9] Build and run fixed-point modulation-math tests\n'
"${CXX}" "${COMMON_FLAGS[@]}" \
  firmware/sf2/Sf2ModulationMath.cpp \
  tests/sf2/test_sf2_modulation_math.cpp \
  -o "${BUILD_DIR}/test_sf2_modulation_math"
"${BUILD_DIR}/test_sf2_modulation_math"

printf '\n[6/9] Build and run per-zone modulator resolver tests\n'
"${CXX}" "${COMMON_FLAGS[@]}" \
  firmware/sf2/Sf2PdtaReader.cpp \
  firmware/sf2/Sf2ZoneModulatorResolver.cpp \
  tests/sf2/test_sf2_zone_modulator_resolver.cpp \
  -o "${BUILD_DIR}/test_sf2_zone_modulator_resolver"
"${BUILD_DIR}/test_sf2_zone_modulator_resolver"

printf '\n[7/9] Build and run playback materializer tests\n'
"${CXX}" "${COMMON_FLAGS[@]}" \
  firmware/voice/PitchMath.cpp \
  firmware/sf2/Sf2PlaybackMaterializer.cpp \
  tests/sf2/test_sf2_playback_materializer.cpp \
  -o "${BUILD_DIR}/test_sf2_playback_materializer"
"${BUILD_DIR}/test_sf2_playback_materializer"

printf '\n[8/9] Build and run SF2 note-on modulation runtime tests\n'
"${CXX}" "${COMMON_FLAGS[@]}" -pthread \
  firmware/sample/PublishedSampleDirectory.cpp \
  firmware/sample/ProgramSnapshot.cpp \
  firmware/voice/VoiceRenderer.cpp \
  firmware/voice/PitchMath.cpp \
  firmware/sf2/Sf2ModulationMath.cpp \
  firmware/sf2/Sf2PlaybackMaterializer.cpp \
  firmware/gm/PlaybackProgramSnapshot.cpp \
  firmware/gm/ZoneResolver.cpp \
  tests/sf2/test_sf2_note_on_modulation_runtime.cpp \
  -o "${BUILD_DIR}/test_sf2_note_on_modulation_runtime"
"${BUILD_DIR}/test_sf2_note_on_modulation_runtime"

printf '\n[9/9] Build and run playback program prepare tests\n'
"${CXX}" "${COMMON_FLAGS[@]}" \
  firmware/sf2/Sf2PdtaReader.cpp \
  firmware/sf2/Sf2ResolvedZone.cpp \
  firmware/sf2/Sf2ZoneModulatorResolver.cpp \
  firmware/voice/PitchMath.cpp \
  firmware/sf2/Sf2PlaybackMaterializer.cpp \
  firmware/sf2/Sf2PlaybackProgramPreparer.cpp \
  tests/sf2/test_sf2_playback_program_preparer.cpp \
  -o "${BUILD_DIR}/test_sf2_playback_program_preparer"
"${BUILD_DIR}/test_sf2_playback_program_preparer"

printf '\nPhase G host validation passed.\n'
