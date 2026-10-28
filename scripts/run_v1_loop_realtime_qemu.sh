#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"
: "${IDF_PATH:?Activate ESP-IDF v6.1 first}"
MODE="${1:-on}"
[[ "$MODE" == on || "$MODE" == off ]] || exit 2
OUT="$ROOT/build/v1-loop-realtime-qemu-$MODE"
python "$IDF_PATH/tools/idf_tools.py" install qemu-xtensa
QEMU_BIN="$(find "${IDF_TOOLS_PATH:-$HOME/.espressif}/tools/qemu-xtensa" -type f -name qemu-system-xtensa -perm -u+x -print | sort | tail -n 1)"
test -x "$QEMU_BIN"
"$QEMU_BIN" --version | tee "$OUT/qemu-version.txt"
python "$IDF_PATH/tools/idf.py" -B "$OUT/target" merge-bin -o qemu_flash.bin --pad-to-size 16MB
set +e
timeout --signal=TERM 120s "$QEMU_BIN" -nographic -machine esp32s3 \
    -drive "file=$OUT/target/qemu_flash.bin,if=mtd,format=raw" -nic none > "$OUT/qemu.log" 2>&1
status=$?
set -e
cat "$OUT/qemu.log"
if [[ "$status" != 0 && "$status" != 124 && "$status" != 143 ]]; then exit "$status"; fi
ANALYSIS=0
[[ "$MODE" == off ]] || ANALYSIS=1
python -m tools.v1_loop_realtime_evidence --native build/v1-loop-realtime-host/native.log \
    --target "$OUT/qemu.log" --profile qemu --analysis "$ANALYSIS" --output "$OUT/execution.json"
