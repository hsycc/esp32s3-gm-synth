#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"
: "${IDF_PATH:?Activate the pinned ESP-IDF v6.1 environment}"
PROFILE="${1:-qemu}"
ANALYSIS="${2:-off}"
OUTPUT_MODE="${3:-bypass}"
[[ "$PROFILE" == qemu || "$PROFILE" == physical ]] || { echo 'Use qemu or physical'; exit 2; }
[[ "$ANALYSIS" == on || "$ANALYSIS" == off ]] || { echo 'Use on or off'; exit 2; }
[[ "$OUTPUT_MODE" == bypass || "$OUTPUT_MODE" == peak ]] || { echo 'Use bypass or peak'; exit 2; }
OUT="$ROOT/build/v1-loop-realtime-$PROFILE-$ANALYSIS-$OUTPUT_MODE"
# Check the toolchain before moving any previous build or its evidence.
IDF_VERSION="$(python "$IDF_PATH/tools/idf.py" --version)"
printf '%s\n' "$IDF_VERSION"
[[ "$IDF_VERSION" == 'ESP-IDF v6.1' ]] || { echo 'Expected ESP-IDF v6.1' >&2; exit 2; }
# A retry starts from the pinned defaults, while retaining the complete prior
# attempt (including immutable bundles and runtime logs). A CI-created build.log
# alone is not a configured attempt and must stay in the current output directory.
for PREVIOUS in generated target sdkconfig flash-bundle build-audit.json atomic-backend-audit.json execution.json; do
    if [[ -e "$OUT/$PREVIOUS" || -L "$OUT/$PREVIOUS" ]]; then
        HISTORY="$ROOT/build/v1-loop-realtime-history"
        mkdir -p "$HISTORY"
        ARCHIVE="$(mktemp -d "$HISTORY/$PROFILE-$ANALYSIS-$OUTPUT_MODE.XXXXXXXX")"
        mv "$OUT" "$ARCHIVE/build"
        printf 'Previous build preserved: %s\n' "$ARCHIVE/build"
        break
    fi
done
mkdir -p "$OUT/generated"
printf 'Build output: %s\n' "$OUT"
printf '%s\n' "$IDF_VERSION" > "$OUT/idf-version.txt"
if [[ "$PROFILE" == qemu ]]; then
    python3 tests/app/make_loop_replay_fixture.py "$OUT/fixture"
    SOURCE="$OUT/fixture/fixture.sf2"
else
    SOURCE="$ROOT/assets/sf2/subsets/FM_GM_Open_subset_6MiB.sf2"
fi
# Keep argv nonempty: Bash 3.2 rejects an empty array under set -u.
set -- "$SOURCE" "$OUT/generated" --profile "$PROFILE" --output-mode "$OUTPUT_MODE"
[[ "$ANALYSIS" == off ]] || set -- "$@" --analysis
python3 -m tools.v1_loop_realtime_manifest "$@"
python "$IDF_PATH/tools/idf.py" -B "$OUT/target" -D IDF_TARGET=esp32s3 \
    -D "SDKCONFIG=$OUT/sdkconfig" \
    -D "SDKCONFIG_DEFAULTS=$ROOT/profiles/v1-loop-realtime/sdkconfig.common;$ROOT/profiles/v1-loop-realtime/sdkconfig.$PROFILE" \
    -D GM_SYNTH_BUILD_LOOP_REALTIME_QUAL=ON \
    -D "GM_SYNTH_REALTIME_GENERATED=$OUT/generated" build
python -m tools.v1_loop_target_evidence build --root "$ROOT" \
    --database "$OUT/target/compile_commands.json" --elf "$OUT/target/esp32s3_gm_synth.elf" \
    --app-prefix main --output "$OUT/build-audit.json"
python -m tools.v1_atomic_backend_audit --idf "$IDF_PATH" --build "$OUT/target" \
    --sdkconfig "$OUT/sdkconfig" --profile "$PROFILE" --output "$OUT/atomic-backend-audit.json"
python -m tools.v1_loop_realtime_bundle --root "$ROOT" --build "$OUT/target" \
    --generated "$OUT/generated" --sdkconfig "$OUT/sdkconfig" --output "$OUT/flash-bundle"
