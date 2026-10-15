# ESP32-S3 GM Synth

Experimental/product-oriented software GM sound module for ESP32-S3.

The project targets a validated GM/SF2 profile and a realtime audio architecture suitable for embedded products rather than unrestricted desktop-class SF2 compatibility.

## Hardware baseline

The product baseline is fixed to **ESP32-S3 N16R8**:

- 16 MB on-module flash
- 8 MB Octal PSRAM
- ESP32-S3 @ 240 MHz
- ESP-IDF v6.1
- initial PCM residency arena: 4 MiB

R16 is not a target and must not be used to justify memory decisions.

## Initial targets

- 44.1 kHz, 16-bit stereo I2S output
- GM1: 128 melodic programs + drums
- 16 MIDI channels
- 16 voices qualification target
- 18–20 voices stretch target only when measured realtime budget permits
- native `.sf2` as a first-class user asset
- shared/per-program PSRAM-backed sample residency
- 64-frame audio blocks as the initial baseline
- global send FX (reverb / chorus)
- realtime event queue with intra-block scheduling
- adaptive polyphony and runtime deadline diagnostics

SoundFont file size is not itself the synth-engine limit. Runtime resident working set, storage latency, PSRAM reserve and realtime DSP cost are the relevant constraints.

## Compatibility / qualification banks

The repository uses multiple real banks because no single SoundFont is the specification:

- FM/GM Open SoundFont — simple/open GM baseline
- GeneralUser GS — real-world generator/modulator baseline
- FluidR3_GM — larger classic GM / partial-residency stress baseline
- TimGM6mb — compact real-world N16R8/modulator-heavy baseline
- GMBK5X128 — non-standard robustness/stress input

Deterministic ~6 MiB fixtures under `assets/sf2/subsets/` drive CI and residency tests. The pinned FM/GM fixture is also used by the ESP32-S3 QEMU and physical-Flash storage benchmark so both paths operate on identical SF2 bytes.

See `docs/SF2_BANK_ANALYSIS.md` and `docs/SF2_COMPATIBILITY.md`.

## Current architecture

The M3 control/storage path is now implemented as:

```text
native .sf2
  -> VFS RandomAccessReader
  -> Sf2SampleSource
  -> Program dependency set
  -> ProgramPrepareController
  -> direct-to-PSRAM chunked loading
  -> Program Pins
  -> PublishedSampleDirectory
  -> immutable READY ProgramSnapshot
  -> block-boundary Commit
  -> move-only Voice Ref
```

The residency/admission design intentionally supports SF2 banks whose complete PCM does not fit in 8 MiB PSRAM. Full residency is an optional optimization, not an architectural prerequisite.

## Validation layers

The project keeps three different validation layers separate:

1. **Host tests** — SF2 semantics, residency ownership, failure rollback, publication, Program Prepare, VFS reader and benchmark logic.
2. **ESP32-S3 QEMU** — Xtensa/ESP-IDF/FreeRTOS integration, 8 MiB Octal PSRAM semantics, generated Flash FATFS, real SF2 parsing and VFS -> PSRAM execution.
3. **Physical ESP32-S3 N16R8** — actual PSRAM reserve, internal-Flash/SD throughput and tail latency, DSP CPU cost, I2S deadlines and qualified polyphony.

QEMU throughput is never treated as physical hardware performance evidence.

The first complete QEMU real-SF2 storage path passed in `esp-idf-build #21`; see `benchmarks/QEMU_SF2_STORAGE_BASELINE_2026-10-15.md`.

A dedicated physical N16R8 internal-Flash benchmark build is available through:

```bash
./scripts/build_hwbench_idf.sh <PORT>
idf.py -B build-hwbench -p <PORT> monitor
```

It runs the same pinned real SF2 through 4/8/16/32/64 KiB reads, with 4 MiB read per pass. See `docs/HARDWARE_BRINGUP_N16R8.md`.

## Repository layout

```text
.
├── AGENTS.md
├── .agent/PLANS.md
├── docs/
├── firmware/
│   ├── platform/
│   ├── audio/
│   ├── midi/
│   ├── gm/
│   ├── sf2/
│   ├── sample/
│   ├── synth/
│   ├── dsp/
│   ├── storage/
│   └── diagnostics/
├── tools/
│   ├── sf2_analyzer/
│   ├── sf2_conformance/
│   ├── sf2_subset/
│   ├── residency_fixture/
│   └── storage_benchmark/
├── tests/
├── benchmarks/
└── scripts/
```

## Development model

Cloud/host CI covers host tests, deterministic SF2 analysis/conformance, ESP32-S3 cross-builds, QEMU integration and construction of the physical benchmark firmware image.

Measurements that require the actual electrical/storage/audio characteristics of an N16R8 board remain on-device work: physical PSRAM reserve, Flash/SD timing, I2S underruns, render deadlines and stable polyphony.

## Status

**M1/M2/M2.5 are established and CI-backed. M3 software/integration is complete through the ESP32-S3 QEMU real-SF2 path.**

The current M3 gate is physical N16R8 evidence:

- boot/PSRAM reserve on the exact module;
- 4 MiB internal-Flash -> PSRAM sweep with p95/p99/max latency;
- SD -> PSRAM comparison if SD is selected for the product;
- on-target Program Prepare/cache-eviction timing.

M4 Voice DSP/I2S integration starts after this storage/memory baseline is captured, so storage faults and realtime audio faults remain separable.
