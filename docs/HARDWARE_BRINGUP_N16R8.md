# ESP32-S3 N16R8 Hardware Bring-up / M6 Qualification

## Purpose

This document describes the remaining physical-hardware gate for M6 after the
host GM128 semantic/residency/lifetime qualification completed.

Reference target:

- ESP32-S3-WROOM-1-N16R8 class module
- 16 MiB flash, DIO, 80 MHz
- 8 MiB Octal PSRAM, 80 MHz
- ESP-IDF v6.1
- CPU 240 MHz
- normal firmware PCM arena: 4 MiB
- dedicated M6 qualification-image PCM arena: 4.5 MiB
- audio render block: 64 frames at 44.1 kHz (`~1451 us` budget)

The 4.5 MiB value is **not** a production-default change. Host full-bank
qualification established it as the first tested arena that admits the
representative 16-channel Active + worst READY transaction. The physical image
exists to determine whether that arena leaves enough total N16R8 PSRAM/SRAM
headroom for the rest of the runtime.

See:

- `benchmarks/GM128_HOST_QUALIFICATION_BASELINE_2026-10-18.md`
- `benchmarks/N16R8_FLASH_STORAGE_BASELINE_2026-10-15.md`
- `tasks/M6_GM128_VALIDATION.md`

## 1. CI prerequisite

Do not use an arbitrary commit for hardware qualification. The corresponding
GitHub `esp-idf-build` workflow must first prove:

- normal production/default build still uses a 4 MiB PCM arena;
- ESP32-S3 / N16R8 configuration checks pass;
- QEMU boot succeeds;
- real-SF2 raw-FATFS integration succeeds;
- Program Prepare functional telemetry parses;
- render telemetry parses for `1/4/8/12/16/18/20/24` Voices;
- the dedicated physical M6 qualification image builds successfully.

Host/QEMU timing is regression evidence only. Physical N16R8 measurements are
the performance oracle.

## 2. Build one M6 qualification image

Activate the repository-pinned ESP-IDF v6.1 environment, then build:

```bash
. "$HOME/esp/esp-idf/export.sh"
./scripts/build_hwbench_idf.sh
```

The generated image is deliberately separate from the normal firmware. Its
configuration must contain:

```text
CONFIG_GM_SYNTH_PCM_ARENA_BYTES=4718592
CONFIG_GM_SYNTH_STORAGE_BENCHMARK_ENABLE=y
CONFIG_GM_SYNTH_STORAGE_BENCHMARK_BYTES=4194304
CONFIG_GM_SYNTH_PROGRAM_PREPARE_BENCHMARK_ENABLE=y
CONFIG_GM_SYNTH_RENDER_BENCHMARK_ENABLE=y
CONFIG_GM_SYNTH_PHYSICAL_QUALIFICATION_BUILD=y
```

The checked-in raw FATFS fixture remains the pinned
`FM_GM_Open_subset_6MiB.sf2` (`5,898,214` bytes, 67 playable samples). It is used
for repeatable physical storage/Prepare timing. The larger full FM bank is used
by host GM128 qualification; it does not fit the current 8 MiB benchmark FATFS
partition and is not required to reproduce the hardware timing path.

## 3. Flash once and capture one complete serial log

Build and flash directly:

```bash
./scripts/build_hwbench_idf.sh <PORT>
idf.py -B build-hwbench -p <PORT> monitor
```

Save the complete first-boot log without editing values. One run should contain
platform memory, storage, Program Prepare and render timing evidence.

### Platform / memory markers

Capture every line beginning with or containing:

```text
Detected physical PSRAM
Requesting PCM residency arena
PSRAM before arena
INTERNAL_HEAP before arena
PSRAM after arena
INTERNAL_HEAP after arena
PSRAM after Program Prepare
INTERNAL_HEAP after Program Prepare
PSRAM after render benchmark
INTERNAL_HEAP after render benchmark
M6_QUAL_MEMORY
```

Required physical checks:

- detected PSRAM is consistent with 8 MiB R8;
- 4.5 MiB contiguous arena allocation succeeds;
- residency initializes over exactly `4,718,592` bytes;
- no PSRAM/cache/reboot/Guru Meditation errors;
- total/free/minimum/largest-free PSRAM is recorded before and after each phase;
- total/free/minimum/largest-free internal SRAM is recorded before and after each phase;
- final `M6_QUAL_MEMORY` shows zero stale-reference faults.

Do not decide the production arena from nominal 8 MiB capacity. The remaining
PSRAM after Voice/DSP/FX/runtime static allocations and minimum-free telemetry is
what matters.

## 4. Internal Flash -> PSRAM storage evidence

The physical image keeps the established 4 MiB storage sweep so results remain
directly comparable with the 2026-10-15 baseline.

Expected source:

```text
path             = /sf2/test.sf2
file bytes       = 5898214
playable samples = 67
benchmark bytes  = 4194304
chunk sizes      = 4096 / 8192 / 16384 / 32768 / 65536
```

For every chunk size record:

- MiB/s;
- elapsed time;
- min / average / p50 / p95 / p99 / max latency.

The established baseline was roughly 5.8–6.3 MiB/s and justified retaining a
32 KiB source read chunk. New numbers should be compared for regression, not
used as an audio-thread streaming target: filesystem I/O remains prohibited in
the audio hot path.

## 5. Program Prepare physical evidence

The same boot then exercises the source-backed Program Prepare benchmark.
Record the complete `program_prepare_bench:` telemetry, including:

- cold Prepare latency and bytes loaded;
- warm/reused Prepare latency;
- deterministic cache pressure;
- safe LRU eviction count/bytes;
- commit/retire timing;
- release-tail reference survival;
- capacity/allocation failures;
- residency high-water values.

All storage work must remain in the control/storage domain.

## 6. Voice DSP render matrix

The qualification image measures the production render path in 64-frame blocks
using deterministic looping PCM at fixed Voice counts:

```text
1, 4, 8, 12, 16, 18, 20, 24 Voices
```

The boot log must contain:

```text
render_bench: START ... timing_scope=hardware physical_timing_evidence=1
render_bench: case voices=1 ...
render_bench: case voices=4 ...
...
render_bench: case voices=24 ...
render_bench: PASS ... timing_scope=hardware physical_timing_evidence=1
```

For each Voice count record:

- min / average / p50 / p95 / p99 / max render time;
- deadline misses against the 1451 us block budget;
- clipped samples;
- deterministic checksum.

### Interpreting the Voice matrix

A case is not automatically a guaranteed product polyphony merely because one
short benchmark pass completes. The useful first cut is:

- **comfortable**: p99 and max remain materially below 1451 us, zero misses;
- **borderline**: p99 approaches the budget or isolated max values consume most
  of the margin;
- **failed**: deadline misses occur or max repeatedly exceeds the block budget.

Final guaranteed polyphony still requires the physical long-running/worst-path
stress listed below.

## 7. What this image does not qualify

The qualification image intentionally does **not** start physical I2S output.
Therefore it cannot prove:

- I2S DMA underrun count;
- end-to-end audio-task scheduling jitter;
- codec/I2S integration;
- final audible quality.

Do not report "zero I2S underruns" from this image. That gate begins only after
the actual I2S output task/DMA path is connected.

Likewise, the deterministic render benchmark exercises the real scheduler,
Voice mixer and global effects processor, but it is not a substitute for the
full-FM mixed-MIDI physical stress workload.

## 8. Final physical stress before declaring guaranteed polyphony

After the one-shot qualification image establishes a safe arena and candidate
Voice count, run the actual audio path with:

- dense sustained piano + sustain pedal;
- repeated same-note retrigger;
- drum bursts;
- release-tail/pad accumulation;
- mixed 16-channel MIDI;
- repeated Program Change between materially different working sets;
- representative filter/modulation/effects-heavy paths;
- long-running playback.

Record:

- active / peak / admission Voice count;
- render p95 / p99 / max;
- deadline misses;
- I2S underruns;
- event queue high-water and drops;
- invalid sample/fetch accesses;
- Voice steals / transient steal tails;
- internal SRAM minimum/largest-free;
- total PSRAM minimum/largest-free;
- sample-arena used/high-water/fragmentation.

Only after this physical stress is green may a stable Voice count be called
"guaranteed" and the final physical release benchmark be committed under
`benchmarks/`.

## 9. Pass / fail boundary for the next board run

The next board run is useful if it answers these questions in one log:

1. Does a 4.5 MiB contiguous PCM arena allocate cleanly on the actual N16R8?
2. What PSRAM and internal-SRAM reserve remains after Prepare and Render?
3. What are cold/warm/eviction Prepare latencies?
4. Which of 16 / 18 / 20 Voices retain comfortable 64-frame deadline margin?
5. Are there any deadline, stale-reference, allocation, cache or reboot faults?

If 4.5 MiB leaves insufficient runtime reserve, keep/reconsider the arena and
residency policy rather than forcing the allocation. If render timing becomes
borderline before 20 Voices, use the measured stable case as the candidate for
the final I2S stress suite.
