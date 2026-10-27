#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)";cd "$ROOT"
: "${IDF_PATH:?Activate pinned ESP-IDF v6.1}"
OUT="$ROOT/build/v1-loop-app-target"
mkdir -p "$OUT"
python "$IDF_PATH/tools/idf.py" --version | tee "$OUT/idf-version.txt"
grep -Eq 'ESP-IDF v6\.1($|[.-])' "$OUT/idf-version.txt"
python3 tests/app/generate_loop_app_embedded.py "$OUT/generated"
python "$IDF_PATH/tools/idf.py" -C "$ROOT/tests/idf/loop_app" -B "$OUT/target" \
  -D "SDKCONFIG=$OUT/sdkconfig.target" -D "SDKCONFIG_DEFAULTS=$ROOT/tests/idf/loop_app/sdkconfig.defaults" \
  -D "V1_APP_FIXTURE_DIR=$OUT/generated" -D IDF_TARGET=esp32s3 build
python -m tools.v1_loop_target_evidence build --root "$ROOT" \
  --database "$OUT/target/compile_commands.json" --elf "$OUT/target/v1_loop_app_target.elf" \
  --app-prefix tests/idf/loop_app/main --output "$OUT/target-build-audit.json"
