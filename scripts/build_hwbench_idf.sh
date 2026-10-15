#!/usr/bin/env bash
set -Eeuo pipefail

: "${IDF_PATH:?IDF_PATH must point to the pinned ESP-IDF v6.1 checkout}"

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="$ROOT_DIR/build-hwbench"
SDKCONFIG_PATH="$ROOT_DIR/sdkconfig.hwbench"
STAGING_DIR="$ROOT_DIR/.qemu_sf2_data"
FIXTURE_SOURCE="$ROOT_DIR/assets/sf2/subsets/FM_GM_Open_subset_6MiB.sf2"
FIXTURE_TARGET="$STAGING_DIR/test.sf2"
PORT="${1:-}"

cd "$ROOT_DIR"

echo "== Verify pinned real-SF2 fixture =="
grep 'assets/sf2/subsets/FM_GM_Open_subset_6MiB.sf2$' \
    assets/sf2/manifests/subset-sha256.txt | sha256sum -c -

rm -rf "$BUILD_DIR" "$STAGING_DIR" "$SDKCONFIG_PATH" "$ROOT_DIR/sdkconfig.hwbench.old"
mkdir -p "$STAGING_DIR"
cp "$FIXTURE_SOURCE" "$FIXTURE_TARGET"

fixture_bytes="$(stat -c '%s' "$FIXTURE_TARGET" 2>/dev/null || stat -f '%z' "$FIXTURE_TARGET")"
if [[ "$fixture_bytes" -ne 5898214 ]]; then
    echo "Unexpected FM/GM hardware-benchmark fixture size: $fixture_bytes" >&2
    exit 1
fi

echo "== Configure ESP32-S3 N16R8 hardware benchmark =="
idf.py -B "$BUILD_DIR" \
    -D "SDKCONFIG=$SDKCONFIG_PATH" \
    -D "SDKCONFIG_DEFAULTS=$ROOT_DIR/sdkconfig.defaults;$ROOT_DIR/sdkconfig.hwbench.defaults" \
    set-target esp32s3

echo "== Build firmware + raw FATFS fixture =="
idf.py -B "$BUILD_DIR" build

echo "== Verify hardware benchmark configuration =="
grep -q '^CONFIG_IDF_TARGET="esp32s3"$' "$SDKCONFIG_PATH"
grep -q '^CONFIG_ESPTOOLPY_FLASHSIZE_16MB=y$' "$SDKCONFIG_PATH"
grep -q '^CONFIG_SPIRAM_MODE_OCT=y$' "$SDKCONFIG_PATH"
grep -q '^CONFIG_GM_SYNTH_STORAGE_BENCHMARK_ENABLE=y$' "$SDKCONFIG_PATH"
grep -q '^CONFIG_GM_SYNTH_STORAGE_BENCHMARK_PATH="/sf2/test.sf2"$' "$SDKCONFIG_PATH"
grep -q '^CONFIG_GM_SYNTH_STORAGE_BENCHMARK_BYTES=4194304$' "$SDKCONFIG_PATH"
grep -q '^CONFIG_GM_SYNTH_QEMU_FLASH_SF2_TEST=y$' "$SDKCONFIG_PATH"

if [[ ! -f "$BUILD_DIR/qemu_sf2.bin" ]]; then
    echo "Expected raw FATFS fixture was not generated: $BUILD_DIR/qemu_sf2.bin" >&2
    exit 1
fi

echo
echo "Hardware benchmark image is ready."
echo "  build dir : $BUILD_DIR"
echo "  sdkconfig : $SDKCONFIG_PATH"
echo "  fixture   : $fixture_bytes bytes"
echo "  sweep     : 4 MiB per chunk size (4/8/16/32/64 KiB)"

if [[ -n "$PORT" ]]; then
    echo
    echo "== Flash hardware benchmark to $PORT =="
    idf.py -B "$BUILD_DIR" -p "$PORT" flash
    echo
    echo "Flash complete. Start the serial monitor with:"
    echo "  idf.py -B build-hwbench -p '$PORT' monitor"
else
    echo
    echo "To flash later:"
    echo "  ./scripts/build_hwbench_idf.sh <PORT>"
    echo "or:"
    echo "  idf.py -B build-hwbench -p <PORT> flash monitor"
fi
