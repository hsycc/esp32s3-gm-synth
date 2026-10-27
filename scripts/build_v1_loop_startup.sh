#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"
: "${IDF_PATH:?Activate the pinned ESP-IDF v6.1 environment}"
PROFILE="${1:-qemu}"; ANALYSIS="${2:-off}"
[[ "$PROFILE" == qemu || "$PROFILE" == physical ]] || { echo 'Use qemu or physical';exit 2; }
[[ "$ANALYSIS" == on || "$ANALYSIS" == off ]] || { echo 'Use on or off';exit 2; }
OUT="$ROOT/build/v1-loop-startup-$PROFILE-$ANALYSIS"
# Never wipe any previous/user build. A dedicated profile cannot be reused with
# a different SDK config silently; remove this particular directory explicitly.
[[ ! -f "$OUT/target/CMakeCache.txt" ]] || { echo "Existing build: $OUT; use a fresh directory";exit 2; }
mkdir -p "$OUT/generated"
if [[ "$PROFILE" == qemu ]];then
    python3 tests/app/make_loop_analysis_fixture.py "$OUT/fixture"
    SOURCE="$OUT/fixture/fixture.sf2"
else
    SOURCE="$ROOT/assets/sf2/subsets/FM_GM_Open_subset_6MiB.sf2"
fi
EXTRA=();[[ "$ANALYSIS" == off ]] || EXTRA+=(--analysis)
python3 -m tools.v1_loop_startup_manifest "$SOURCE" "$OUT/generated" --profile "$PROFILE" "${EXTRA[@]}"
python "$IDF_PATH/tools/idf.py" --version | tee "$OUT/idf-version.txt"
grep -qx "ESP-IDF v6.1" "$OUT/idf-version.txt"
# Specify all defaults explicitly; do not inherit the root physical defaults in QEMU.
python "$IDF_PATH/tools/idf.py" -B "$OUT/target" -D IDF_TARGET=esp32s3 \
    -D "SDKCONFIG=$OUT/sdkconfig" \
    -D "SDKCONFIG_DEFAULTS=$ROOT/profiles/v1-loop-startup/sdkconfig.common;$ROOT/profiles/v1-loop-startup/sdkconfig.$PROFILE" \
    -D GM_SYNTH_BUILD_LOOP_STARTUP_QUAL=ON -D "GM_SYNTH_STARTUP_GENERATED=$OUT/generated" build
python -m tools.v1_loop_target_evidence build --root "$ROOT" \
    --database "$OUT/target/compile_commands.json" --elf "$OUT/target/esp32s3_gm_synth.elf" \
    --app-prefix main --output "$OUT/build-audit.json"
