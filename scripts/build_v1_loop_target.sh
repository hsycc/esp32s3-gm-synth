#!/usr/bin/env bash
# Isolated qualification directories only; never changes normal sdkconfig or M7.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"
: "${IDF_PATH:?Activate ESP-IDF v6.1 first}"
OUT="$ROOT/build/v1-loop-target"
mkdir -p "$OUT"
python "$IDF_PATH/tools/idf.py" --version | tee "$OUT/idf-version.txt"
grep -Eq 'ESP-IDF v6\.1($|[.-])' "$OUT/idf-version.txt"
# Full production app, uniform ABI enabled, but NO individual plan provisioning.
python "$IDF_PATH/tools/idf.py" -B "$OUT/firmware" \
  -D "SDKCONFIG=$OUT/sdkconfig.firmware" \
  -D "SDKCONFIG_DEFAULTS=$ROOT/sdkconfig.defaults" -D IDF_TARGET=esp32s3 \
  -D 'CMAKE_CXX_FLAGS=-DGM_SYNTH_EXPERIMENTAL_VOICE_LOOP=1' build
python -m tools.v1_loop_target_evidence build --root "$ROOT" \
  --database "$OUT/firmware/compile_commands.json" \
  --elf "$OUT/firmware/esp32s3_gm_synth.elf" --app-prefix main \
  --output "$OUT/firmware-build-audit.json"

# Separate small app uses synthetic PCM; it is not a playable release image.
python "$IDF_PATH/tools/idf.py" -C "$ROOT/tests/idf/loop_voice" -B "$OUT/smoke" \
  -D "SDKCONFIG=$OUT/sdkconfig.smoke" \
  -D "SDKCONFIG_DEFAULTS=$ROOT/tests/idf/loop_voice/sdkconfig.defaults" \
  -D IDF_TARGET=esp32s3 build
python -m tools.v1_loop_target_evidence build --root "$ROOT" \
  --database "$OUT/smoke/compile_commands.json" \
  --elf "$OUT/smoke/v1_loop_voice_target.elf" --app-prefix tests/idf/loop_voice/main \
  --output "$OUT/smoke-build-audit.json"
