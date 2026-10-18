#!/usr/bin/env bash
set -Eeuo pipefail

: "${IDF_PATH:?IDF_PATH must point to the pinned ESP-IDF v6.1 checkout}"

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="$ROOT_DIR/build-hwbench"
SDKCONFIG_PATH="$ROOT_DIR/sdkconfig.hwbench"
STAGING_DIR="$ROOT_DIR/.qemu_sf2_data"
FIXTURE_SOURCE="$ROOT_DIR/assets/sf2/subsets/FM_GM_Open_subset_6MiB.sf2"
FIXTURE_TARGET="$STAGING_DIR/test.sf2"
FIXTURE_SHA256="9650eb1c2202e2d92aa3be155e4e895cf6531a241c8ea413fc09e7f9159d4a62"
PORT="${1:-}"
IDF_PY="$IDF_PATH/tools/idf.py"

if [[ ! -f "$IDF_PY" ]]; then
    echo "ESP-IDF idf.py not found: $IDF_PY" >&2
    exit 1
fi

if [[ -n "${IDF_PYTHON_ENV_PATH:-}" && -x "$IDF_PYTHON_ENV_PATH/bin/python" ]]; then
    IDF_PYTHON="$IDF_PYTHON_ENV_PATH/bin/python"
elif command -v python3 >/dev/null 2>&1; then
    IDF_PYTHON="$(command -v python3)"
elif command -v python >/dev/null 2>&1; then
    IDF_PYTHON="$(command -v python)"
else
    echo "Python interpreter not found for ESP-IDF." >&2
    exit 1
fi

run_idf() {
    "$IDF_PYTHON" "$IDF_PY" "$@"
}

cd "$ROOT_DIR"

echo "== ESP-IDF environment =="
echo "IDF_PATH=$IDF_PATH"
echo "IDF_PYTHON=$IDF_PYTHON"
run_idf --version

echo "== Verify pinned real-SF2 fixture =="
if command -v sha256sum >/dev/null 2>&1; then
    actual_sha="$(sha256sum "$FIXTURE_SOURCE" | awk '{print $1}')"
elif command -v shasum >/dev/null 2>&1; then
    actual_sha="$(shasum -a 256 "$FIXTURE_SOURCE" | awk '{print $1}')"
else
    actual_sha="$("$IDF_PYTHON" - "$FIXTURE_SOURCE" <<'PY'
import hashlib
import pathlib
import sys

path = pathlib.Path(sys.argv[1])
h = hashlib.sha256()
with path.open('rb') as f:
    for chunk in iter(lambda: f.read(1024 * 1024), b''):
        h.update(chunk)
print(h.hexdigest())
PY
)"
fi

if [[ "$actual_sha" != "$FIXTURE_SHA256" ]]; then
    echo "Unexpected FM/GM hardware-benchmark fixture SHA-256:" >&2
    echo "  expected: $FIXTURE_SHA256" >&2
    echo "  actual  : $actual_sha" >&2
    exit 1
fi

echo "$actual_sha  assets/sf2/subsets/FM_GM_Open_subset_6MiB.sf2"

rm -rf "$BUILD_DIR" "$STAGING_DIR" "$SDKCONFIG_PATH" "$ROOT_DIR/sdkconfig.hwbench.old"
mkdir -p "$STAGING_DIR"
cp "$FIXTURE_SOURCE" "$FIXTURE_TARGET"

fixture_bytes="$(wc -c < "$FIXTURE_TARGET" | tr -d '[:space:]')"
if [[ "$fixture_bytes" -ne 5898214 ]]; then
    echo "Unexpected FM/GM hardware-benchmark fixture size: $fixture_bytes" >&2
    exit 1
fi

echo "== Configure ESP32-S3 N16R8 M6 + Korvo audio qualification image =="
run_idf -B "$BUILD_DIR" \
    -D "SDKCONFIG=$SDKCONFIG_PATH" \
    -D "SDKCONFIG_DEFAULTS=$ROOT_DIR/sdkconfig.defaults;$ROOT_DIR/sdkconfig.hwbench.defaults" \
    set-target esp32s3

echo "== Build firmware + raw FATFS fixture =="
run_idf -B "$BUILD_DIR" build

echo "== Verify M6 qualification configuration =="
grep -q '^CONFIG_IDF_TARGET="esp32s3"$' "$SDKCONFIG_PATH"
grep -q '^CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ_240=y$' "$SDKCONFIG_PATH"
grep -q '^CONFIG_COMPILER_OPTIMIZATION_PERF=y$' "$SDKCONFIG_PATH"
if grep -q '^CONFIG_COMPILER_OPTIMIZATION_DEBUG=y$' "$SDKCONFIG_PATH"; then
    echo "Physical timing qualification must not use DEBUG/-Og optimization" >&2
    exit 1
fi
grep -q '^CONFIG_ESPTOOLPY_FLASHMODE_DIO=y$' "$SDKCONFIG_PATH"
grep -q '^CONFIG_ESPTOOLPY_FLASHFREQ_80M=y$' "$SDKCONFIG_PATH"
grep -q '^CONFIG_ESPTOOLPY_FLASHSIZE_16MB=y$' "$SDKCONFIG_PATH"
grep -q '^CONFIG_SPIRAM_MODE_OCT=y$' "$SDKCONFIG_PATH"
grep -q '^CONFIG_SPIRAM_SPEED_80M=y$' "$SDKCONFIG_PATH"
grep -q '^CONFIG_SPIRAM_ALLOW_BSS_SEG_EXTERNAL_MEMORY=y$' "$SDKCONFIG_PATH"
grep -q '^CONFIG_GM_SYNTH_PCM_ARENA_BYTES=4718592$' "$SDKCONFIG_PATH"
grep -q '^CONFIG_GM_SYNTH_STORAGE_BENCHMARK_ENABLE=y$' "$SDKCONFIG_PATH"
grep -q '^CONFIG_GM_SYNTH_STORAGE_BENCHMARK_PATH="/sf2/test.sf2"$' "$SDKCONFIG_PATH"
grep -q '^CONFIG_GM_SYNTH_STORAGE_BENCHMARK_BYTES=4194304$' "$SDKCONFIG_PATH"
grep -q '^CONFIG_GM_SYNTH_PROGRAM_PREPARE_BENCHMARK_ENABLE=y$' "$SDKCONFIG_PATH"
grep -q '^CONFIG_GM_SYNTH_RENDER_BENCHMARK_ENABLE=y$' "$SDKCONFIG_PATH"
grep -q '^CONFIG_GM_SYNTH_PHYSICAL_QUALIFICATION_BUILD=y$' "$SDKCONFIG_PATH"
grep -q '^CONFIG_GM_SYNTH_QEMU_FLASH_SF2_TEST=y$' "$SDKCONFIG_PATH"

# The dedicated hardware image is also our CI compile gate for the real Korvo
# V4 playback path. Fail loudly if a defaults/config merge silently disables or
# remaps it: a green hwbench build must compile ES8311 + I2S/DMA integration.
grep -q '^CONFIG_GM_SYNTH_I2S_QUAL_ENABLE=y$' "$SDKCONFIG_PATH"
grep -q '^CONFIG_GM_SYNTH_I2S_QUAL_MCLK_GPIO=16$' "$SDKCONFIG_PATH"
grep -q '^CONFIG_GM_SYNTH_I2S_QUAL_BCLK_GPIO=9$' "$SDKCONFIG_PATH"
grep -q '^CONFIG_GM_SYNTH_I2S_QUAL_WS_GPIO=45$' "$SDKCONFIG_PATH"
grep -q '^CONFIG_GM_SYNTH_I2S_QUAL_DOUT_GPIO=8$' "$SDKCONFIG_PATH"
grep -q '^CONFIG_GM_SYNTH_I2S_QUAL_DMA_DESC_NUM=8$' "$SDKCONFIG_PATH"
grep -q '^CONFIG_GM_SYNTH_I2S_QUAL_DMA_FRAME_NUM=64$' "$SDKCONFIG_PATH"
grep -q '^CONFIG_GM_SYNTH_I2S_QUAL_TASK_CORE=1$' "$SDKCONFIG_PATH"
grep -q '^CONFIG_GM_SYNTH_I2S_QUAL_TASK_PRIORITY=20$' "$SDKCONFIG_PATH"

if [[ ! -f "$BUILD_DIR/qemu_sf2.bin" ]]; then
    echo "Expected raw FATFS fixture was not generated: $BUILD_DIR/qemu_sf2.bin" >&2
    exit 1
fi

if [[ -f "$BUILD_DIR/flash_args" ]]; then
    grep -q -- '--flash-mode dio' "$BUILD_DIR/flash_args"
    grep -q -- '--flash-freq 80m' "$BUILD_DIR/flash_args"
    grep -q -- '--flash-size 16MB' "$BUILD_DIR/flash_args"
    grep -q '^0x210000 qemu_sf2.bin$' "$BUILD_DIR/flash_args"
fi

echo
echo "N16R8 M6 + Korvo V4 audio qualification image is ready."
echo "  build dir : $BUILD_DIR"
echo "  sdkconfig : $SDKCONFIG_PATH"
echo "  compiler  : PERF (-O2 base), O3 realtime hot files, assertions enabled"
echo "  flash     : DIO / 80 MHz / 16 MiB"
echo "  fixture   : $fixture_bytes bytes"
echo "  PCM arena : 4.50 MiB (4718592 bytes)"
echo "  storage   : 4 MiB per chunk size (4/8/16/32/64 KiB)"
echo "  prepare   : cold/warm + cache pressure/LRU + commit/retire/release-tail"
echo "  render    : 1/4/8/12/16/18/20/24 Voices, 64-frame blocks"
echo "  memory    : PSRAM + internal-SRAM snapshots before/after Prepare/Render"
echo "  audio     : ES8311 + I2S/DMA qualification enabled"
echo "  I2S pins  : MCLK16 / BCLK9 / WS45 / DOUT8, stereo16 frame with mono duplication"

if [[ -n "$PORT" ]]; then
    echo
    echo "== Flash M6 qualification image to $PORT =="
    run_idf -B "$BUILD_DIR" -p "$PORT" flash
    echo
    echo "Flash complete. Start the serial monitor with:"
    echo "  mkdir -p reports/local"
    echo "  \"$IDF_PYTHON\" \"$IDF_PY\" -B build-hwbench -p '$PORT' monitor | tee reports/local/n16r8-m6.log"
else
    echo
    echo "To flash later:"
    echo "  ./scripts/build_hwbench_idf.sh <PORT>"
    echo "or:"
    echo "  \"$IDF_PYTHON\" \"$IDF_PY\" -B build-hwbench -p <PORT> flash monitor"
fi
