#!/usr/bin/env bash
set -euo pipefail

: "${IDF_PATH:?IDF_PATH must point to the pinned ESP-IDF checkout}"

REPORT_PATH="${1:-reports/qemu-smoke.log}"
TIMEOUT_SECONDS="${QEMU_TIMEOUT_SECONDS:-25}"
TOOLS_ROOT="${IDF_TOOLS_PATH:-$HOME/.espressif}"

mkdir -p "$(dirname "$REPORT_PATH")"

# QEMU is an optional ESP-IDF tool. Install the Espressif-pinned Xtensa package
# that matches the active IDF checkout rather than using the host distro QEMU.
python "$IDF_PATH/tools/idf_tools.py" install qemu-xtensa

QEMU_BIN="$(find "$TOOLS_ROOT/tools/qemu-xtensa" -type f -name qemu-system-xtensa -perm -u+x -print 2>/dev/null | sort | tail -n 1)"
if [[ -z "$QEMU_BIN" || ! -x "$QEMU_BIN" ]]; then
    echo "Unable to locate Espressif qemu-system-xtensa under $TOOLS_ROOT/tools/qemu-xtensa" >&2
    exit 1
fi

echo "Using QEMU: $QEMU_BIN"
"$QEMU_BIN" --version | head -n 1

# QEMU consumes one complete SPI flash image. idf.py merge-bin runs esptool in
# the build directory and pads the image to the configured N16 flash capacity.
idf.py merge-bin -o qemu_flash.bin --pad-to-size 16MB
if [[ ! -f build/qemu_flash.bin ]]; then
    echo "Expected merged QEMU flash image was not produced" >&2
    exit 1
fi

set +e
timeout --signal=TERM "${TIMEOUT_SECONDS}s" \
    "$QEMU_BIN" \
    -nographic \
    -machine esp32s3 \
    -m 8M \
    -global driver=ssi_psram,property=is_octal,value=true \
    -global driver=timer.esp32c3.timg,property=wdt_disable,value=true \
    -drive file=build/qemu_flash.bin,if=mtd,format=raw \
    -nic none \
    2>&1 | tee "$REPORT_PATH"
qemu_status=${PIPESTATUS[0]}
set -e

# The bring-up firmware intentionally reaches idle and keeps running, so timeout
# is the expected termination mechanism after the boot assertions have printed.
if [[ $qemu_status -ne 0 && $qemu_status -ne 124 && $qemu_status -ne 143 ]]; then
    echo "QEMU exited unexpectedly with status $qemu_status" >&2
    exit "$qemu_status"
fi

require_log() {
    local pattern="$1"
    if ! grep -Fq "$pattern" "$REPORT_PATH"; then
        echo "Missing expected QEMU boot log: $pattern" >&2
        exit 1
    fi
}

# Keep smoke assertions independent of the current milestone label (M3/M6/etc.).
# This gate validates platform bring-up; milestone-specific behavior belongs in
# the dedicated integration/benchmark gates below it.
require_log "ESP32-S3 GM synth"
require_log "bring-up"
require_log "Detected physical PSRAM: 8388608 bytes (8.00 MiB)"
require_log "Requesting PCM residency arena: 4194304 bytes (4.00 MiB)"
require_log "Residency ready: capacity=4194304 used=0"
require_log "core linked successfully"

if grep -Fq "Detected physical PSRAM is below 7 MiB" "$REPORT_PATH"; then
    echo "QEMU did not expose the expected 8 MiB PSRAM baseline" >&2
    exit 1
fi

if grep -Eq "Guru Meditation|assert failed|abort\(\)|panic'ed" "$REPORT_PATH"; then
    echo "QEMU log contains an ESP32 panic/assert signature" >&2
    exit 1
fi

echo "ESP32-S3 QEMU smoke test passed: 8 MiB Octal PSRAM + 4 MiB residency arena"
