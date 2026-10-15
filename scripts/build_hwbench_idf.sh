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

cd "$ROOT_DIR"

echo "== Verify pinned real-SF2 fixture =="
if command -v sha256sum >/dev/null 2>&1; then
    actual_sha="$(sha256sum "$FIXTURE_SOURCE" | awk '{print $1}')"
elif command -v shasum >/dev/null 2>&1; then
    actual_sha="$(shasum -a 256 "$FIXTURE_SOURCE" | awk '{print $1}')"
else
    actual_sha="$(python - "$FIXTURE_SOURCE" <<'PY'
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
