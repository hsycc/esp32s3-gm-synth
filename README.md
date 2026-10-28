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

## Original starting targets (historical)

The 16 / 18–20 Voice numbers below describe the initial project target. The
later M6–M8 physical baseline is 24 normal/full-FX and 8 heavy-DSP sample Voices;
the new optional Loop path requires its own physical qualification.

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

The first physical N16R8-class internal-Flash -> PSRAM baseline is also complete; see `benchmarks/N16R8_FLASH_STORAGE_BASELINE_2026-10-15.md`.

Measured physical baseline:

- 8 MiB Octal PSRAM detected at 80 MHz;
- 4 MiB PCM arena allocation succeeds and leaves about 4 MiB PSRAM free at clean bring-up;
- internal Flash -> PSRAM throughput is about 5.8–6.3 MiB/s over 4/8/16/32/64 KiB chunks;
- the current 32 KiB SF2 source chunk reaches 6.184 MiB/s with 5.052 ms P99 per read;
- 32 KiB is retained because 64 KiB gains only about 1.6% aggregate throughput while roughly doubling per-read tail latency;
- the 4 MiB arena remains the baseline until M4 Voice/DSP/I2S memory high-water is measured.

A dedicated physical N16R8 internal-Flash benchmark build remains available through:

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

Measurements that require the actual electrical/storage/audio characteristics of an N16R8 board remain on-device work: Program Prepare/cache timing, optional SD timing, I2S underruns, render deadlines and stable polyphony.

## Status — M0–M8 integrated development baseline (October 2026)

- **M0–M6 core implementation:** host/ESP-IDF and physical qualification complete for the documented N16R8 configuration.
- **M7 GM128/native SF2:** program-level release matrices, representative multichannel behavior and real-hardware long runs are qualified for tested banks; not a claim of arbitrary SF2 support or subjective listening approval.
- **M8 hot-path and volume controls:** the core integration is in `main`. The frozen `bb31908b` image completed three physical stress runs with zero hard-gate failures and unchanged PCM checksum. Its actual `bb31908-dirty` banner is retained and was expressly accepted for integration. Digital master defaults to unity; ES8311 DAC remains -12 dB.
- **Optional Loop path:** PRs #19–#24 are merged; #25 merged as `51dc57d42057e9c23d3cf593717f56cc3841c92e` after all 17 workflows / 25 check runs passed at `5502e974`. That serial startup qualification is distinct from concurrent audio/I2S or hardware performance. The frozen scoring policy is unchanged, and feature defaults remain OFF.
- **Current integration work:** `docs/V1_LOOP_REALTIME_INTEGRATION.md` describes the separate control/audio owner, continuous I2S profile and new-path hardware evidence gates. The earlier 24/8 baseline is not an approval of this enabled path.
- **V1 Release Candidate:** representative listening, output-level/headroom checks, new-path physical qualification and release packaging remain separate gates. No public V1 release is approved yet.

The physical qualification output is **mono downmix via ES8311**, not a spatial-stereo speaker test. 24 normal/full-FX *sample voices* (not necessarily 24 keys on layered presets) and 8 pathological heavy-DSP voices are the measured stability envelope. Do not advertise the software 32-voice cap as a guaranteed realtime specification.

See `benchmarks/M8_PHYSICAL_THREE_RUN_2026-10-24.md`, `docs/V1_RELEASE_CLOSEOUT.md`, `docs/V1_AUDIBLE_ACCEPTANCE.md`, and `.agent/PLANS.md` for evidence and remaining gates.
