#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)";cd "$ROOT"
: "${IDF_PATH:?Activate ESP-IDF v6.1}"
OUT="$ROOT/build/v1-loop-app-target"
bash scripts/test_v1_loop_app_target.sh | tee "$OUT/native.log"
python "$IDF_PATH/tools/idf_tools.py" install qemu-xtensa
TOOLS="${IDF_TOOLS_PATH:-$HOME/.espressif}"
QEMU="$(find "$TOOLS/tools/qemu-xtensa" -type f -name qemu-system-xtensa -perm -u+x -print | sort | tail -n 1)"
test -x "$QEMU";"$QEMU" --version | tee "$OUT/qemu-version.txt"
python "$IDF_PATH/tools/idf.py" -C "$ROOT/tests/idf/loop_app" -B "$OUT/target" merge-bin -o qemu_flash.bin --pad-to-size 4MB
set +e
timeout --signal=TERM 45s "$QEMU" -nographic -machine esp32s3 \
 -drive "file=$OUT/target/qemu_flash.bin,if=mtd,format=raw" -nic none > "$OUT/qemu.log" 2>&1
status=$?
set -e
cat "$OUT/qemu.log"
if [[ "$status" != 0 && "$status" != 124 && "$status" != 143 ]]; then exit "$status"; fi
python -m tools.v1_loop_app_target_evidence --native "$OUT/native.log" --target "$OUT/qemu.log" --output "$OUT/app-target-execution.json"
