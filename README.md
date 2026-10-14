# ESP32-S3 GM Synth

Experimental/product-oriented software GM sound module for ESP32-S3.

The project targets a fixed, validated GM SoundFont bank and a realtime audio architecture suitable for embedded products rather than arbitrary desktop-class SF2 compatibility.

## Initial targets

- ESP32-S3 @ 240 MHz
- 44.1 kHz, 16-bit stereo I2S output
- GM1: 128 melodic programs + drums
- 16 MIDI channels
- 16 voices guaranteed
- 18–20 voices target when realtime budget permits
- Fixed/validated SF2 bank (initial reference size: ~14 MB)
- PSRAM-backed sample residency
- 64-frame audio blocks as the initial baseline
- Global send FX (reverb / chorus)
- Realtime event queue with intra-block scheduling
- Adaptive polyphony and runtime deadline diagnostics

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

Cloud/host tasks should cover builds, static analysis, unit tests, SF2 analysis and code review. Hardware-only validation (I2S output, PSRAM behavior, underruns and realtime timing on a physical ESP32-S3) remains a local/on-device step.

## Status

Architecture/bootstrap phase. No production firmware implementation has been committed yet.
