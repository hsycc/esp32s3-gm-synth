# ESP32-S3 N16R8 Hardware Bring-up

This checklist is the first real-hardware gate for M0 / M3 Phase C.

Reference target:
- ESP32-S3-WROOM-1-N16R8 class module
- 16 MiB flash
- 8 MiB octal PSRAM
- ESP-IDF v6.1
- 240 MHz CPU
- initial PCM residency arena: 4 MiB

The first board run is intentionally limited to platform, storage and residency validation. I2S, MIDI, Voice DSP and effects are not part of this gate.

## Current status

The QEMU functional gate is complete on commit `86e492b` / `esp-idf-build #21`:

- real SF2 fixture generated into raw FATFS;
- 16 MiB virtual flash image boots on ESP32-S3 QEMU;
- 8 MiB Octal PSRAM is exposed;
- `/sf2/test.sf2` mounts and opens through ESP-IDF VFS;
- the real SF2 parses as 67 playable samples;
- the 4/8/16/32/64 KiB storage sweep completes;
- the 4 MiB residency arena initializes after the sweep.

See `benchmarks/QEMU_SF2_STORAGE_BASELINE_2026-10-15.md` for the exact QEMU telemetry. Those throughput numbers are functional telemetry only and are not hardware performance data.

## 1. Build the normal firmware

Use the repository-pinned ESP-IDF release:

```bash
. "$HOME/esp/esp-idf/export.sh"
./scripts/build_idf.sh
```

Expected build target:

```text
IDF_TARGET = esp32s3
Flash      = 16 MB
PSRAM      = enabled, octal, 80 MHz
PCM arena  = 4194304 bytes
```

The GitHub `esp-idf-build` workflow must also pass before using a commit as a hardware baseline.

## 2. First boot: platform / PSRAM gate

Typical local flow:

```bash
idf.py -p <PORT> flash monitor
```

Keep the complete first-boot serial log. Do not edit the reported memory numbers before attaching them to a benchmark/report.

A passing first boot must show all of the following:

1. firmware identifies itself as the M0/M3 bring-up target;
2. ESP-IDF version is printed;
3. detected PSRAM is consistent with an R8-class module;
4. the largest PSRAM free block is at least the configured 4 MiB arena request;
5. the PCM arena allocation succeeds;
6. arena address satisfies `SampleResidencyManager::kAlignment`;
7. `SampleResidencyManager::begin()` succeeds over the PSRAM arena;
8. post-allocation PSRAM telemetry is printed;
9. residency telemetry reports zero used bytes and one large initial free span;
10. there is no reboot loop, cache error or PSRAM initialization warning.

Record the exact serial-log values for:

```text
ESP-IDF version
PSRAM total before arena
PSRAM free before arena
PSRAM largest free block before arena
PSRAM minimum free before arena
Requested PCM arena bytes
PSRAM free after arena
PSRAM largest free block after arena
Residency capacity bytes
Residency largest free span bytes
```

These measurements decide whether the production arena remains 4 MiB or can safely move toward 5–6 MiB. Do not infer this from the nominal 8 MiB module size alone.

## 3. Internal Flash -> PSRAM benchmark

The repository now has a dedicated real-hardware benchmark build which reuses the exact SF2 fixture already validated in QEMU.

Build only:

```bash
. "$HOME/esp/esp-idf/export.sh"
./scripts/build_hwbench_idf.sh
```

Build and flash directly:

```bash
./scripts/build_hwbench_idf.sh <PORT>
idf.py -B build-hwbench -p <PORT> monitor
```

The hardware benchmark is intentionally separate from the production/default build. It uses:

```text
SF2 fixture        = FM_GM_Open_subset_6MiB.sf2
fixture bytes      = 5898214
playable samples   = 67
PCM arena          = 4194304 bytes
benchmark bytes    = 4194304 bytes per chunk-size pass
chunk sizes        = 4096 / 8192 / 16384 / 32768 / 65536 bytes
storage path       = /sf2/test.sf2
```

The temporary hardware benchmark currently reuses the configuration symbol `GM_SYNTH_QEMU_FLASH_SF2_TEST`; despite that historical name, the code path itself is only a raw-flash FATFS fixture + read-only VFS mount and is valid on physical ESP32-S3 hardware.

### Required benchmark evidence

The serial log must contain:

```text
Detected physical PSRAM: 8388608 bytes
QEMU FATFS mounted: base=/sf2 partition=qemu_sf2
Storage benchmark VFS open: path=/sf2/test.sf2 file_bytes=5898214
Storage benchmark SF2 parsed: playable_samples=67
Storage benchmark source: path=/sf2/test.sf2 ... benchmark_bytes=4194304
chunk=4096 bytes=4194304 ...
chunk=8192 bytes=4194304 ...
chunk=16384 bytes=4194304 ...
chunk=32768 bytes=4194304 ...
chunk=65536 bytes=4194304 ...
Residency ready: capacity=4194304 used=0 ...
```

The log prefix still says `QEMU FATFS mounted` because that message predates the hardware benchmark path. This wording is cosmetic; the physical run is valid as long as the firmware was built with `sdkconfig.hwbench.defaults` and flashed to the N16R8 board.

For every chunk size record:

- throughput MiB/s;
- min latency;
- average latency;
- p50 latency;
- p95 latency;
- p99 latency;
- max latency;
- total elapsed time.

Do not compare these hardware numbers against the QEMU 2.3–2.4 MiB/s values as a pass/fail threshold. The QEMU figures are not representative of physical SPI flash timing.

## 4. Hardware pass / fail rule

### Platform pass

- build is clean under ESP-IDF v6.1;
- N16R8 configuration assertions pass;
- 4 MiB SPIRAM arena allocation succeeds on target;
- residency manager initializes successfully;
- measured remaining PSRAM is recorded for later Voice/FX budgeting.

### Storage functional pass

- the pinned real SF2 mounts and opens from physical flash;
- SF2 parser reports 67 playable samples;
- all five 4 MiB benchmark passes complete with status `Ok`;
- benchmark writes directly into the 4 MiB PSRAM arena without a second multi-MiB staging buffer;
- no panic, invalid read, reboot, cache error or residency fault occurs.

### Fail

Treat any of the following as a real platform/storage issue rather than bypassing the check:

- detected PSRAM substantially below the R8 expectation;
- largest free PSRAM block below 4 MiB at clean boot;
- arena allocation failure;
- unexpected Quad/Octal PSRAM mismatch;
- flash-size mismatch;
- SF2 fixture cannot mount/open/parse;
- any storage sweep pass returns a failure status;
- residency initialization failure;
- repeated reset, Guru Meditation, cache or PSRAM errors.

A numeric throughput acceptance threshold will be set only after the first physical N16R8 result is captured. Before that point, inventing a threshold would be less useful than recording the real board baseline.

## 5. SD-card benchmark comes second

Do not mix SD-card bring-up with the first Flash test. First prove the on-module Flash -> PSRAM path on N16R8. Then mount the chosen SD interface and run the same `VfsRandomAccessReader` + `runEspStorageBenchmarkSweep()` path against the same SF2 fixture.

That comparison will answer separately:

1. whether internal Flash is fast enough for program preparation / cache refill;
2. whether SD materially improves throughput;
3. whether SD introduces worse p95/p99/max latency even when average throughput is higher.

## 6. Gate before M4

Only after the physical evidence above is captured:

1. keep the measured 4 MiB arena if the remaining PSRAM margin is healthy, or revise it from evidence;
2. record real Flash -> PSRAM throughput and tail latency;
3. add SD -> PSRAM data if SD is part of the product architecture;
4. re-run Program Prepare / cache-eviction scenarios on target;
5. then begin M4 Voice DSP and I2S output integration.

This ordering keeps storage/PSRAM faults separate from audio-DSP timing faults.
