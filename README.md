# ESP32-S3 GM Synth

Experimental/product-oriented software GM sound module for ESP32-S3.

The project targets a fixed/validated GM SoundFont profile and a realtime audio architecture suitable for embedded products rather than arbitrary desktop-class SF2 compatibility.

## Hardware baseline

The product baseline is fixed to **ESP32-S3 N16R8**:

- 16 MB on-module flash
- 8 MB PSRAM
- ESP32-S3 @ 240 MHz

R16 is not a target and must not be used to justify memory decisions.

## Initial targets

- 44.1 kHz, 16-bit stereo I2S output
- GM1: 128 melodic programs + drums
- 16 MIDI channels
- 16 voices qualification target
- 18–20 voices stretch target only when measured realtime budget permits
- SoundFont size is not a product requirement; runtime working set is
- PSRAM-backed sample residency
- 64-frame audio blocks as the initial baseline
- global send FX (reverb / chorus)
- realtime event queue with intra-block scheduling
- adaptive polyphony and runtime deadline diagnostics

## Candidate SoundFont profiles

Two real candidate banks are currently used to drive architecture decisions:

1. `TimGM6mb.sf2`
   - compact GM bank
   - approximately 5.72 MiB source file
   - approximately 5.47 MiB aligned sample PCM
   - potential full-residency candidate on R8, subject to measured runtime reserve
   - significant instrument-modulator usage
2. `GMBK5X128.sf2`
   - approximately 15.06 MiB source file with many extra banks/presets
   - approximately 14.84 MiB aligned sample PCM in the untrimmed source
   - cannot be fully resident on R8
   - raw source is also too large to treat as a practical in-flash product asset on N16 alongside firmware
   - GM-only offline pruning plus shared/per-program residency is the intended evaluation path

The project must support banks whose complete PCM does not fit PSRAM. Full residency is an optimization, not an architectural prerequisite.

See `docs/SF2_BANK_ANALYSIS.md` for measured details.

## Reference projects

This project intentionally combines ideas from three existing ESP32-S3 sampler/synth projects:

1. `copych/ESP32_SF2_Sampler_Synthesizer`
   - SF2 parsing and zone resolution
   - GM/MIDI channel state
   - PSRAM sample pool
   - voice engine and effects routing
2. `copych/ESP32_SD_Sampler`
   - adaptive polyphony
   - smooth voice stealing / fast release
   - streaming diagnostics and overload handling
3. `RealTimeAudioLab/RTAL-EAI-004-Phoenix-Sound-Sampler`
   - realtime task architecture
   - timestamped event scheduling
   - intra-block event placement
   - deadline measurement
   - SRAM/PSRAM placement methodology
   - stress-validation discipline

See `docs/ARCHITECTURE.md` and `docs/REFERENCES.md` for details.

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
│   └── sf2_analyzer/
├── tests/
├── benchmarks/
├── tasks/
└── scripts/
```

## Development model

The repository is designed to be usable both locally and by Codex Cloud.

Cloud/host tasks should cover builds, static analysis, unit tests, SF2 analysis and code review. Hardware-only validation (I2S output, PSRAM behavior, underruns and realtime timing on a physical ESP32-S3 N16R8) remains a local/on-device step.

## Status

M1 SF2 analysis is active. Two real candidate GM banks have been measured; residency architecture will be designed for the N16R8 constraint rather than for a fixed SF2 file size.
