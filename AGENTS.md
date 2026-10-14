# AGENTS.md

## Project purpose

Build a production-oriented GM1 software synthesizer for ESP32-S3 using a fixed, validated SF2 bank.

The firmware is a realtime embedded audio system. Correct realtime behavior has higher priority than architectural elegance, feature count, or desktop-style abstractions.

## Target baseline

- MCU: ESP32-S3 @ 240 MHz
- Output: 44.1 kHz / 16-bit / stereo I2S
- Audio block: 64 frames initially
- MIDI: 16 channels
- GM: 128 melodic programs + channel-10 drums
- Guaranteed polyphony target: 16 voices
- Stretch target: 18–20 voices when measured realtime budget permits
- Sample storage: flash or filesystem-backed SF2/bank assets
- Runtime PCM: PSRAM-resident by default
- Initial reference SoundFont size: approximately 14 MB

These values are targets, not assumptions. Hardware measurements override them.

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

## Program / asset changes

Do not implement Program Change by performing asset I/O inside the audio task.

Preferred model:

1. Prepare next program/assets outside the audio realtime domain.
2. Mark prepared state READY.
3. Commit pointers/indices at an audio block boundary.
4. Retire/release old assets safely after no voice can reference them.

If the complete fixed GM bank fits PSRAM, prefer startup residency and index-only Program Change.

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
1. Correctly support the fixed validated GM bank.
2. Document which SF2 generators/features that bank actually uses.
3. Add broader SF2 compatibility only when it does not compromise realtime behavior.

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

Physical-board verification should include:
- block render time distribution
- peak render time
- audio deadline misses
- underruns
- event queue drops
- sample fetch faults
- lock/contention misses
- PSRAM/internal SRAM usage
- dense MIDI / sustain / program-change stress tests

Never infer true ESP32-S3 realtime capacity solely from host benchmarks.

## Change discipline

- Prefer small, reviewable commits.
- Preserve behavior during structural refactors before optimizing.
- Add measurements before and after performance-sensitive changes.
- Keep reference-code provenance clear in comments/docs when substantial logic is adapted.
- Update architecture or milestone docs when a design decision materially changes.
