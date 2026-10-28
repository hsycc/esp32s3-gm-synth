# V1 PSRAM-enabled handoff atomic contract

Scope: PR #25, based on c596f9e0. This is a construction/storage correction,
not a new atomic implementation, scoring change, runtime opt-in, hardware
performance result, or permission to move realtime metadata into PSRAM.

## Why the always-lock-free type assertion failed

ESP-IDF v6.1 / ESP32-S3 GCC with PSRAM selects
`CONFIG_STDATOMIC_S32C1I_SPIRAM_WORKAROUND=y` and
`-mdisable-hardware-atomics`. The compiler cannot promise every possible
32-bit atomic address is lock-free. IDF's `esp_libc` dispatches addresses
outside `[SOC_EXTRAM_DATA_LOW, SOC_EXTRAM_DATA_HIGH)` to separately compiled
hardware routines; external addresses take a critical-section fallback.

Primary source paths, pinned to the official `espressif/esp-idf` v6.1 tag:
- `components/esp_libc/Kconfig`
- `components/esp_libc/priv_include/esp_stdatomic.h`
- `components/esp_libc/src/stdatomic.c`
- `components/esp_libc/src/port/xtensa/stdatomic_s32c1i.c`
- `components/esp_libc/CMakeLists.txt`
- `components/esp_hw_support/include/esp_memory_utils.h`

## Replacement contract

`RealtimeAtomicContract.h` checks each actual phase word at construction in
`PreparedLoopHandoff` and `LoopProgramChannel`, before publication. It requires
four-byte atomic representation, natural alignment, and the ENTIRE word in
ordinary internal DRAM. It also excludes the exact IDF external virtual range.
This intentionally rejects RTC/IRAM and reclaimed DROM aliases, even if some
are accepted by `esp_ptr_internal()` or internal-capability allocators.

Invalid placement aborts even under NDEBUG, rather than silently entering a
locking realtime path. These objects cannot move/copy after construction.
This fail-fast construction rule is a programming contract, not a recoverable
sample-plan rejection or a concurrent teardown mechanism.

Only ESP32-S3 GCC on IDF6.1 may use the audited address-qualified exception.
Other builds retain the original compile-time always-lock-free requirement.
Changing the SDK series requires a fresh audit. `std::atomic<uint32_t>` fields,
all acquire/release/relaxed operations, object layouts and transaction ordering
are unchanged. No per-block address check is added. The PSRAM workaround and
its compiler flag remain enabled. There is no inline-assembly substitute or
private SDK include in the application.

## Evidence gates

The existing startup workflow runs the address tests and both native lifecycle
suites. Host address-stub tests cover alignment, full-word range boundaries,
external aliases, NDEBUG abort and unsupported SDK/target rejection. These
are not physical PSRAM tests. Reversible source guards accept ONLY the two
constructor checks/includes and replacement of the original type assertions;
restored protocol bodies must still equal their frozen versions byte-for-byte.

For the physical build, `v1_atomic_backend_audit.py` expands compiler response
files, preserves SDK source hashes, checks normal/workaround compile flags,
checks source-level address routing, and disassembles the actual IDF archive's
four-byte hardware load/store routines. Missing evidence, calls/traps or unknown
or backwards branches fail the audit. The external locking implementation is
not removed. QEMU-on/off continue exercising the complete root startup scenario.

This establishes only the two new phase-word storage/backend contract. It does
not qualify every existing atomic elsewhere, guarantee a wall-clock bound,
prove physical RAM fit, or replace real PSRAM/I2S/DMA/concurrent-audio tests.
Normal feature defaults remain OFF and runtime_approved remains false.
