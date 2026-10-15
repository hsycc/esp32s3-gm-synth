# Physical N16R8 Internal-Flash -> PSRAM Baseline — 2026-10-15

## Scope

This report records the first physical ESP32-S3 N16R8-class board run of the generated Flash FATFS -> ESP-IDF VFS -> real SF2 parser -> PSRAM benchmark path.

Unlike the QEMU report, these numbers are physical-hardware storage/memory evidence and may be used to guide the M3 storage/residency baseline.

## Source

- firmware commit: `7124f27` (`build: make hwbench script work with function-based idf activation`)
- ESP-IDF: v6.1
- chip revision: v0.2
- CPU: 240 MHz
- flash: 16 MiB, DIO, 80 MHz
- flash chip detected by ESP-IDF: GigaDevice (`gd`)
- PSRAM: 8 MiB Octal, 80 MHz
- PSRAM vendor ID: `0x0d` (AP)
- physical board/module: N16R8-class target; exact board/module SKU should still be recorded separately for final qualification
- PCM residency arena: 4 MiB
- storage medium: on-module SPI flash
- filesystem/path: generated raw FATFS, mounted read-only at `/sf2`, file `/sf2/test.sf2`

## SF2 fixture

- file: `assets/sf2/subsets/FM_GM_Open_subset_6MiB.sf2`
- SHA-256: `9650eb1c2202e2d92aa3be155e4e895cf6531a241c8ea413fc09e7f9159d4a62`
- file bytes: `5,898,214`
- playable samples: `67`
- `smpl` payload offset: `544`
- `smpl` payload bytes: `5,887,930`
- benchmark bytes per chunk-size pass: `4,194,304` (4 MiB)

## Boot / memory result

PASS:

- detected physical PSRAM: `8,388,608` bytes
- flash boot mode: DIO / 80 MHz / 16 MiB
- 4 MiB PCM arena allocation succeeded
- SF2 FATFS partition mounted successfully
- real SF2 opened and parsed successfully
- all 4/8/16/32/64 KiB benchmark passes completed
- residency manager initialized after the benchmark
- no panic/assert/Guru Meditation/reboot/cache/storage error observed

PSRAM telemetry:

| Stage | Total | Free | Largest free block | Minimum free |
|---|---:|---:|---:|---:|
| Before arena | 8,388,608 | 8,386,156 | 8,257,536 | 8,386,156 |
| After 4 MiB arena | 8,388,608 | 4,185,336 | 4,128,768 | 4,180,720 |

Residency after benchmark:

```text
capacity=4194304
used=0
largest_free_span=4194304
free_spans=1
```

The 4 MiB arena leaves about 4.0 MiB of PSRAM free at clean bring-up, with a largest remaining free block of about 3.94 MiB. That is a healthy enough margin to keep 4 MiB as the current production baseline while M4 Voice/DSP/I2S memory use is still unknown.

## Physical storage telemetry

| Chunk | Reads | Bytes | Elapsed | Throughput | Min us | Avg us | P50 us | P95 us | P99 us | Max us |
|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 4 KiB | 1024 | 4,194,304 | 688,609 us | 5.809 MiB/s | 608 | 671 | 659 | 737 | 740 | 1,305 |
| 8 KiB | 512 | 4,194,304 | 665,543 us | 6.010 MiB/s | 1,266 | 1,298 | 1,281 | 1,359 | 1,364 | 2,118 |
| 16 KiB | 256 | 4,194,304 | 652,247 us | 6.133 MiB/s | 2,510 | 2,546 | 2,568 | 2,579 | 2,582 | 3,239 |
| 32 KiB | 128 | 4,194,304 | 646,862 us | 6.184 MiB/s | 5,042 | 5,052 | 5,047 | 5,051 | 5,052 | 5,815 |
| 64 KiB | 64 | 4,194,304 | 636,706 us | 6.282 MiB/s | 9,929 | 9,947 | 9,936 | 9,939 | 10,669 | 10,669 |

## Interpretation

### 1. Internal Flash -> PSRAM path is viable for control-domain sample preparation

Physical throughput is approximately `5.8–6.3 MiB/s` across the tested chunk sizes. A full 4 MiB sequential refill takes approximately `0.64–0.69 s` depending on chunk size.

This is suitable for asynchronous/control-domain Program Prepare and cache refill. It is not suitable for filesystem reads in the audio hot path, which remains prohibited by the architecture.

### 2. Keep the current 32 KiB SF2 read chunk

`Sf2SampleSource::kReadChunkBytes` is currently 32 KiB.

On this board:

- 32 KiB: `6.184 MiB/s`, P99 `5.052 ms`, max `5.815 ms`
- 64 KiB: `6.282 MiB/s`, P99/max `10.669 ms`

The 32 KiB path is only about 1.6% slower in aggregate throughput than 64 KiB while roughly halving per-read tail latency. The entire 4 MiB pass differs by only about 10 ms (`646.862 ms` vs `636.706 ms`).

Decision: **retain 32 KiB as the default SF2 source read chunk**.

### 3. Keep the 4 MiB residency arena for M4 entry

The first physical run does not justify increasing the arena to 5–6 MiB yet. After a 4 MiB arena allocation the board still has approximately 4.0 MiB free PSRAM, but M4 still needs memory for Voice state, DSP/filter/envelopes, audio buffers, effects, queues and diagnostics.

Decision: **keep the 4 MiB PCM residency arena until physical M4 memory high-water measurements exist**.

### 4. QEMU performance numbers are superseded for hardware decisions

The QEMU ~2.3–2.4 MiB/s figures remain useful only as functional integration telemetry. Physical storage decisions should use this report instead.

## M3 next gate

The raw internal-Flash benchmark and clean-boot PSRAM baseline are now complete.

Before starting full M4 audio integration, the remaining useful M3 hardware measurement is on-target Program Prepare/cache behavior using real sample dependencies and the same source-backed residency path:

- cold Program Prepare latency and bytes loaded;
- warm/reused Program Prepare latency;
- rapid Program Change prepare/commit/retire timing;
- cache eviction count/bytes and latency under arena pressure;
- confirmation that all source I/O remains outside the future audio task.

An SD-card comparison is only required before M4 if SD is part of the intended product storage architecture; it is not required to validate the internal-Flash baseline itself.
