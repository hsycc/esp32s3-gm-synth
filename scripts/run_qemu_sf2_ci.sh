#!/usr/bin/env bash
set -Eeuo pipefail

: "${IDF_PATH:?IDF_PATH must point to the pinned ESP-IDF checkout}"

REPORT_PATH="${1:-reports/qemu-sf2-flash.log}"
TIMEOUT_SECONDS="${QEMU_SF2_TIMEOUT_SECONDS:-60}"
TOOLS_ROOT="${IDF_TOOLS_PATH:-$HOME/.espressif}"
ROOT_DIR="$(pwd)"
BUILD_DIR="$ROOT_DIR/build-qemu-sf2"
SDKCONFIG_PATH="$ROOT_DIR/sdkconfig.qemu"
STAGING_DIR="$ROOT_DIR/.qemu_sf2_data"
FIXTURE_SOURCE="$ROOT_DIR/assets/sf2/subsets/FM_GM_Open_subset_6MiB.sf2"
FIXTURE_TARGET="$STAGING_DIR/test.sf2"
QEMU_BENCHMARK_BYTES=262144
PREFLIGHT_REPORT_PATH="${REPORT_PATH%.log}-preflight.log"
CURRENT_STAGE="initialization"

mkdir -p "$(dirname "$REPORT_PATH")"
: > "$PREFLIGHT_REPORT_PATH"
exec > >(tee -a "$PREFLIGHT_REPORT_PATH") 2>&1

on_error() {
    local rc=$?
    echo "ERROR: QEMU SF2 CI failed: status=$rc stage=$CURRENT_STAGE line=${BASH_LINENO[0]} command=${BASH_COMMAND}" >&2
    exit "$rc"
}
trap on_error ERR

stage() {
    CURRENT_STAGE="$1"
    echo
    echo "===== QEMU SF2 CI stage: $CURRENT_STAGE ====="
}

stage "verify real SF2 fixture"
# Pin the real fixture before copying it into the generated FATFS staging
# directory. This prevents a stale/corrupted test asset from silently becoming
# the emulator reference bank.
grep 'assets/sf2/subsets/FM_GM_Open_subset_6MiB.sf2$' \
    assets/sf2/manifests/subset-sha256.txt | sha256sum -c -

stage "stage FATFS source tree"
rm -rf "$BUILD_DIR" "$STAGING_DIR" "$SDKCONFIG_PATH" "$ROOT_DIR/sdkconfig.qemu.old"
mkdir -p "$STAGING_DIR"
cp "$FIXTURE_SOURCE" "$FIXTURE_TARGET"

fixture_bytes="$(stat -c '%s' "$FIXTURE_TARGET")"
if [[ "$fixture_bytes" -ne 5898214 ]]; then
    echo "Unexpected FM/GM QEMU fixture size: $fixture_bytes" >&2
    exit 1
fi

echo "Staged real SF2 fixture: $FIXTURE_TARGET ($fixture_bytes bytes)"

stage "configure dedicated ESP32-S3 QEMU-SF2 build"
# Build a dedicated emulator image. Production/default sdkconfig and partition
# layout stay untouched; this build layers QEMU-only FATFS/benchmark defaults.
idf.py -B "$BUILD_DIR" \
    -D "SDKCONFIG=$SDKCONFIG_PATH" \
    -D "SDKCONFIG_DEFAULTS=$ROOT_DIR/sdkconfig.defaults;$ROOT_DIR/sdkconfig.qemu.defaults" \
    set-target esp32s3

stage "build firmware and FATFS image"
idf.py -B "$BUILD_DIR" build

stage "verify dedicated sdkconfig"
grep -q '^CONFIG_IDF_TARGET="esp32s3"$' "$SDKCONFIG_PATH"
grep -q '^CONFIG_ESPTOOLPY_FLASHSIZE_16MB=y$' "$SDKCONFIG_PATH"
grep -q '^CONFIG_SPIRAM_MODE_OCT=y$' "$SDKCONFIG_PATH"
grep -q '^CONFIG_PARTITION_TABLE_CUSTOM=y$' "$SDKCONFIG_PATH"
grep -q '^CONFIG_PARTITION_TABLE_CUSTOM_FILENAME="partitions_qemu.csv"$' "$SDKCONFIG_PATH"
grep -q '^CONFIG_GM_SYNTH_STORAGE_BENCHMARK_ENABLE=y$' "$SDKCONFIG_PATH"
grep -q '^CONFIG_GM_SYNTH_QEMU_FLASH_SF2_TEST=y$' "$SDKCONFIG_PATH"
grep -q '^CONFIG_GM_SYNTH_STORAGE_BENCHMARK_PATH="/sf2/test.sf2"$' "$SDKCONFIG_PATH"
grep -q "^CONFIG_GM_SYNTH_STORAGE_BENCHMARK_BYTES=${QEMU_BENCHMARK_BYTES}$" "$SDKCONFIG_PATH"

stage "verify generated FATFS image"
echo "QEMU-SF2 build outputs of interest:"
find "$BUILD_DIR" -maxdepth 2 -type f \( -name '*qemu*' -o -name '*fat*' -o -name 'flash_args' -o -name 'flasher_args.json' \) -print | sort || true
if [[ ! -f "$BUILD_DIR/qemu_sf2.bin" ]]; then
    echo "QEMU FATFS image was not generated at $BUILD_DIR/qemu_sf2.bin" >&2
    exit 1
fi

fat_bytes="$(stat -c '%s' "$BUILD_DIR/qemu_sf2.bin")"
echo "Generated QEMU FATFS image: $BUILD_DIR/qemu_sf2.bin ($fat_bytes bytes)"
if [[ -f "$BUILD_DIR/flash_args" ]]; then
    echo "---- dedicated flash_args ----"
    cat "$BUILD_DIR/flash_args"
    echo "------------------------------"
fi

stage "locate Espressif QEMU"
# Reuse/install the Espressif-pinned QEMU package from the active IDF checkout.
python "$IDF_PATH/tools/idf_tools.py" install qemu-xtensa
QEMU_BIN="$(find "$TOOLS_ROOT/tools/qemu-xtensa" -type f -name qemu-system-xtensa -perm -u+x -print 2>/dev/null | sort | tail -n 1)"
if [[ -z "$QEMU_BIN" || ! -x "$QEMU_BIN" ]]; then
    echo "Unable to locate Espressif qemu-system-xtensa under $TOOLS_ROOT/tools/qemu-xtensa" >&2
    exit 1
fi
echo "Using QEMU binary: $QEMU_BIN"

stage "merge 16 MiB virtual flash image"
idf.py -B "$BUILD_DIR" merge-bin -o qemu_sf2_flash.bin --pad-to-size 16MB
FLASH_IMAGE="$BUILD_DIR/qemu_sf2_flash.bin"
if [[ ! -f "$FLASH_IMAGE" ]]; then
    echo "Expected merged QEMU SF2 flash image was not produced: $FLASH_IMAGE" >&2
    exit 1
fi
echo "Merged flash image: $FLASH_IMAGE ($(stat -c '%s' "$FLASH_IMAGE") bytes)"

stage "launch ESP32-S3 QEMU"
set +e
timeout --signal=TERM "${TIMEOUT_SECONDS}s" \
    "$QEMU_BIN" \
    -nographic \
    -machine esp32s3 \
    -m 8M \
    -global driver=ssi_psram,property=is_octal,value=true \
    -global driver=timer.esp32c3.timg,property=wdt_disable,value=true \
    -drive file="$FLASH_IMAGE",if=mtd,format=raw \
    -nic none \
    2>&1 | tee "$REPORT_PATH"
qemu_status=${PIPESTATUS[0]}
set -e

if [[ $qemu_status -ne 0 && $qemu_status -ne 124 && $qemu_status -ne 143 ]]; then
    echo "QEMU SF2 integration run exited unexpectedly with status $qemu_status" >&2
    exit "$qemu_status"
fi

stage "verify QEMU integration logs"
require_log() {
    local pattern="$1"
    if ! grep -Fq "$pattern" "$REPORT_PATH"; then
        echo "Missing expected QEMU SF2 integration log: $pattern" >&2
        echo "---- QEMU SF2 log tail ----" >&2
        tail -n 120 "$REPORT_PATH" >&2 || true
        echo "---------------------------" >&2
        exit 1
    fi
}

require_log "Detected physical PSRAM: 8388608 bytes (8.00 MiB)"
require_log "QEMU FATFS mounted: base=/sf2 partition=qemu_sf2"
require_log "Storage benchmark SF2 parsed: playable_samples=67"
require_log "Storage benchmark source: path=/sf2/test.sf2"
require_log "benchmark_bytes=${QEMU_BENCHMARK_BYTES}"
require_log "chunk=4096 bytes=${QEMU_BENCHMARK_BYTES}"
require_log "chunk=8192 bytes=${QEMU_BENCHMARK_BYTES}"
require_log "chunk=16384 bytes=${QEMU_BENCHMARK_BYTES}"
require_log "chunk=32768 bytes=${QEMU_BENCHMARK_BYTES}"
require_log "chunk=65536 bytes=${QEMU_BENCHMARK_BYTES}"
require_log "Residency ready: capacity=4194304 used=0"
require_log "M3 core linked successfully"

if grep -Eq "QEMU FATFS mount failed|VFS path cannot be opened|SF2 parse failed|fixture sample-count mismatch|One or more storage benchmark chunk-size passes failed|Guru Meditation|assert failed|abort\(\)|panic'ed" "$REPORT_PATH"; then
    echo "QEMU SF2 integration log contains a failure/panic signature" >&2
    tail -n 120 "$REPORT_PATH" >&2 || true
    exit 1
fi

stage "complete"
echo "ESP32-S3 QEMU real-SF2 flash/FATFS integration test passed"
echo "NOTE: emulator throughput/latency values are functional telemetry only, not N16R8 hardware performance data."
