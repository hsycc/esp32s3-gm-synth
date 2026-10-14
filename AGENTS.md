# AGENTS.md

## Project purpose

Build a production-oriented GM1 software synthesizer for ESP32-S3 using validated SF2-derived assets.

The firmware is a realtime embedded audio system. Correct realtime behavior has higher priority than architectural elegance, feature count, or desktop-style abstractions.

## Fixed hardware baseline

- MCU/module class: **ESP32-S3 N16R8**
- Flash: **16 MB**
- PSRAM: **8 MB**
- CPU: ESP32-S3 @ 240 MHz
- Output: 44.1 kHz / 16-bit / stereo I2S
- Audio block: 64 frames initially
- MIDI: 16 channels
- GM: 128 melodic programs + channel-10 drums
- Qualification polyphony target: 16 active sample voices
- Stretch target: 18–20 voices only when measured realtime budget permits

**R16 is not a target.** Do not make design decisions that require 16 MB PSRAM.

SoundFont file size is not itself a requirement. Architecture must be based on actual sample working sets, flash footprint, PSRAM residency and realtime cost.

## Candidate bank profiles

Current real analysis uses two user-supplied candidate SoundFonts:

### TimGM6mb.sf2
- compact source
- about 5.47 MiB aligned PCM across all samples
- possible full-resident optimization on R8, but only if measured system reserve remains safe
- substantial instrument-modulator usage, so compatibility work is non-trivial

### GMBK5X128.sf2
- large source with many extra banks
- about 14.84 MiB aligned PCM in the untrimmed source
- cannot be fully resident on R8
- must not be used raw as an assumed in-flash product asset on N16
- evaluate offline GM-only pruning plus shared/per-program residency
- no effective pmod/imod records observed in the current file, making SF2 semantics simpler than TimGM6mb in that area

Do not commit the source `.sf2` files unless explicitly approved and redistribution rights are known.

## Architectural references

Use these projects as reference implementations, not as unquestioned specifications:

### copych/ESP32_SF2_Sampler_Synthesizer
Primary reference for:
- SF2 RIFF/PDTA/SDTA parsing
- preset / instrument / zone resolution
- GM/MIDI channel state
- PSRAM SamplePool
- sample-based voice DSP
- per-channel state and bus FX

### copych/ESP32_SD_Sampler
Primary reference for:
- adaptive polyphony
- voice-steal fast release / click mitigation
- overload diagnostics
- storage-streaming concepts for future large sample sets

### RealTimeAudioLab/RTAL-EAI-004-Phoenix-Sound-Sampler
Primary reference for:
- task/core separation
- timestamped musical events
- intra-block event scheduling
- realtime deadline measurement
- SRAM vs PSRAM placement based on measurement
- stress validation and release gates

## Realtime rules

### Audio hot path MUST NOT

- allocate or free heap memory
- access filesystem or SD card
- load/parse SF2 data
- write flash/NVS
- print logs
- wait on a blocking mutex
- perform unbounded work
- call APIs with nondeterministic latency when avoidable

### Audio hot path SHOULD

- use fixed-size buffers
- use precomputed control-rate parameters
- use lock-free/SPSC queues where appropriate
- use non-blocking synchronization
- expose render-time and fault counters
- keep expensive control-rate math out of the per-sample loop
- commit prepared state only at safe block boundaries

If a change violates these rules, document the reason and provide measurements proving it is safe.

## Asset strategy

The architecture must support **shared/per-program PSRAM residency** because at least one real candidate bank exceeds R8 capacity.

Full-bank residency is an optional fast path only when a specific compiled bank demonstrably fits R8 with adequate measured reserve.

Preferred product pipeline:

`source SF2 -> host analyzer/compiler -> validated GM asset set -> flash -> PSRAM residency manager -> Voice Engine`

Offline compilation/pruning may remove unused banks/presets, normalize metadata and optionally resample PCM when justified by quality/storage measurements.

## Program / asset changes

Do not implement Program Change by performing asset I/O inside the audio task.

Preferred model:

1. Prepare next program/assets outside the audio realtime domain.
2. Share already-resident samples across channels/programs.
3. Mark prepared state READY.
4. Commit pointers/indices at an audio block boundary.
5. Retire/release old assets safely after no voice can reference them.

If a particular compiled bank fits fully in R8, index-only Program Change is a permitted optimization.

## Event timing

MIDI and internal musical events should eventually follow:

`input -> decoder -> timestamp -> event queue -> audio block scheduler -> intra-block execution`

Do not assume every event may simply be applied at the next block boundary.

## Voice allocation

- One MIDI note may consume multiple voices because SF2 zones/layers can overlap.
- Polyphony means active sample voices, not necessarily held MIDI keys.
- Implement measured/adaptive voice admission rather than treating the compile-time maximum as always safe.
- Prefer short fade/fast-release voice stealing over hard discontinuities.

## SF2 compatibility policy

The project is not initially trying to support every arbitrary SoundFont.

Priority order:
1. Correctly support the selected validated GM asset profile(s).
2. Document which SF2 generators/modulators those source banks actually use.
3. Prefer resolving complex/unsupported source semantics in the offline compiler when that reduces firmware realtime cost without changing intended playback.
4. Add broader runtime SF2 compatibility only when it does not compromise the N16R8 target.

Do not claim full SF2 compatibility without explicit tests for generator semantics, stereo-linked samples, modulators, defaults, loop modes and zone hierarchy.

## Verification

Host/cloud verification should include:
- builds
- unit tests
- SF2 parsing tests
- zone-resolution tests
- MIDI state tests
- deterministic DSP tests where practical
- static analysis

Physical-board verification on N16R8 should include:
- block render time distribution
- peak render time
- audio deadline misses
- underruns
- event queue drops
- sample fetch faults
- lock/contention misses
- PSRAM/internal SRAM usage
- sample-residency pressure
- dense MIDI / sustain / program-change stress tests

Never infer true ESP32-S3 realtime capacity solely from host benchmarks.

## Change discipline

- Prefer small, reviewable commits.
- Preserve behavior during structural refactors before optimizing.
- Add measurements before and after performance-sensitive changes.
- Keep reference-code provenance clear in comments/docs when substantial logic is adapted.
- Update architecture or milestone docs when a design decision materially changes.
