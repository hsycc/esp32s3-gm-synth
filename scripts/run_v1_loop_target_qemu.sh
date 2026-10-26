#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"
: "${IDF_PATH:?Activate ESP-IDF v6.1 first}"
OUT="$ROOT/build/v1-loop-target"
mkdir -p "$OUT"
# Same fixture with native and target compilers; no timing comparison.
"${CXX:-g++}" -std=c++17 -O2 -Wall -Wextra -Werror -pthread -Ifirmware \
  -DGM_SYNTH_EXPERIMENTAL_VOICE_LOOP=1 \
  firmware/sample/PublishedSampleDirectory.cpp firmware/voice/VoiceRenderer.cpp \
  firmware/voice/VoiceEffectsBus.cpp firmware/voice/VoiceRuntimeControl.cpp \
  firmware/voice/GlobalEffectsProcessor.cpp tests/voice/test_loop_voice_target_native.cpp \
  -o "$OUT/native-smoke"
"$OUT/native-smoke" | tee "$OUT/native.log"
python "$IDF_PATH/tools/idf_tools.py" install qemu-xtensa
TOOLS="${IDF_TOOLS_PATH:-$HOME/.espressif}"
QEMU_BIN="$(find "$TOOLS/tools/qemu-xtensa" -type f -name qemu-system-xtensa -perm -u+x -print | sort | tail -n 1)"
test -x "$QEMU_BIN"
"$QEMU_BIN" --version | tee "$OUT/qemu-version.txt"
python "$IDF_PATH/tools/idf.py" -C "$ROOT/tests/idf/loop_voice" -B "$OUT/smoke" \
  merge-bin -o qemu_flash.bin --pad-to-size 4MB
set +e
timeout --signal=TERM 45s "$QEMU_BIN" -nographic -machine esp32s3 \
  -global driver=timer.esp32c3.timg,property=wdt_disable,value=true \
  -drive "file=$OUT/smoke/qemu_flash.bin,if=mtd,format=raw" -nic none \
  > "$OUT/qemu.log" 2>&1
status=$?
set -e
cat "$OUT/qemu.log"
# app_main deliberately returns to idle: timeout is allowed ONLY with markers,
# exact native/target trace agreement and no panic/assert/WDT marker below.
if [[ "$status" != 0 && "$status" != 124 && "$status" != 143 ]]; then exit "$status"; fi
python -m tools.v1_loop_target_evidence logs --native "$OUT/native.log" \
  --target "$OUT/qemu.log" --output "$OUT/qemu-execution.json"
