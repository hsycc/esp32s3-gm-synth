# AGENTS.md

## Project purpose

Build a production-oriented GM/SoundFont software synthesizer for ESP32-S3 where **standard `.sf2` files are first-class user assets**.

Users should be able to import, create, share and redistribute compatible SF2 banks without requiring conversion to a proprietary format. A bundled GM bank is a default/qualification asset, not the only bank the firmware may ever load.

The firmware is a realtime embedded audio system. Correct realtime behavior has higher priority than architectural elegance, feature count, or desktop-style abstractions.

## Fixed hardware baseline

- MCU/module class: **ESP32-S3 N16R8**
- Flash: **16 MB**
- PSRAM: **8 MB**
- CPU: ESP32-S3 @ 240 MHz
- Output: 44.1 kHz / 16-bit / stereo I2S
- Audio block: 64 frames initially
- MIDI: 16 channels
- GM: 128 melodic programs + channel-10 drums for qualification
- Qualification polyphony target: 16 active sample voices
- Stretch target: 18–20 voices only when measured realtime budget permits

**R16 is not a target.** Do not make design decisions that require 16 MB PSRAM.

SoundFont file size is not itself a synthesis-capability metric. Architecture must be based on storage footprint, parsed metadata, active sample working sets, PSRAM residency and realtime CPU cost.

## SF2 product policy

- `.sf2` is the user-facing/import/share format.
- Firmware must be able to parse supported SF2 files directly.
- A custom `.gmb`/compiled bank must not be required for users to share or load a SoundFont.
- Optional derived caches/indexes are allowed if they can be regenerated from the original SF2 and are not part of the sharing contract.
- Prefer a small sidecar index/cache over replacing the source SF2 with a proprietary format.
- SF3 may be investigated later, but SF2 compatibility and ecosystem interoperability take priority for V1.

## Reference / qualification bank classes

Do not use one bank as the definition of the SF2 format.

### Compact real-world baseline — TimGM6mb.sf2
- compact GM bank
- about 5.47 MiB aligned PCM across all samples in the analyzed file
- possible full-resident optimization on R8, but only if measured system reserve remains safe
- substantial instrument-modulator usage, so it is useful for modulator validation

### Open simple-GM baseline — FM/GM Open SoundFont
- public GPL SF2 project
- bank 0 programs 0–127 plus GM standard drums
- 44.1 kHz / 16-bit / mono samples
- deliberately simple generator profile
- useful for parser/GM-map interoperability tests, but not sufficient alone as a full SoundFont-compliance stress test

### Real-world compliance baseline — GeneralUser GS
- GM/GS bank widely used with FluidSynth/BASSMIDI-class engines
- author explicitly documents heavy use of SoundFont synthesis and modulator features
- use as a compatibility reference for generator/modulator semantics
- too large to assume full residency on R8; must exercise partial residency

### Non-standard stress input — GMBK5X128.sf2
- user-supplied non-standard source
- about 14.84 MiB aligned PCM in the untrimmed source
- cannot be fully resident on R8
- useful as a storage/residency stress asset, but **not** as the reference for standard SF2 semantics

Do not commit third-party `.sf2` files unless explicitly approved and redistribution rights are known.

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

Do not inherit its current SF2 feature gaps blindly; in particular, modulator and generator behavior must be validated against standard banks/reference synths.

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

## Asset and residency strategy

The architecture must support **shared/per-program PSRAM residency** because user-loadable SF2 banks may be much larger than R8.

Full-bank residency is only an optional fast path for small banks that demonstrably fit R8 with measured reserve.

Preferred user flow:

`user SF2 -> validate/parse -> optional derived index/cache -> storage -> async sample residency manager -> PSRAM -> Voice Engine`

The source SF2 remains the authoritative asset. An index/cache may precompute:
- preset/bank lookup
- program -> sample dependency sets
- sample offsets/lengths
- normalized/resolved zone metadata where semantics are preserved

Do not require offline proprietary conversion before playback.

## Program / asset changes

Do not implement Program Change by performing asset I/O inside the audio task.

Preferred model:

1. Resolve required zones/samples from parsed/indexed SF2 metadata.
2. Prepare missing samples outside the audio realtime domain.
3. Share already-resident samples across channels/programs.
4. Mark prepared state READY.
5. Commit pointers/indices at an audio block boundary.
6. Retire/release old assets safely after no voice can reference them.

If a particular SF2 bank fits fully in R8, index-only Program Change is a permitted optimization.

## Event timing

MIDI and internal musical events should eventually follow:

`input -> decoder -> timestamp -> event queue -> audio block scheduler -> intra-block execution`

Do not assume every event may simply be applied at the next block boundary.

## Voice allocation

- One MIDI note may consume multiple voices because SF2 zones/layers can overlap.
- Polyphony means active sample voices, not necessarily held MIDI keys.
- Stereo-linked samples may consume additional rendering resources and must be accounted for explicitly.
- Implement measured/adaptive voice admission rather than treating the compile-time maximum as always safe.
- Prefer short fade/fast-release voice stealing over hard discontinuities.

## SF2 compatibility policy

The product should support a clearly documented **SF2 compatibility profile**, not claim arbitrary/full compatibility without evidence.

Priority order:
1. Correct RIFF/PDTA/SDTA parsing and global/local zone hierarchy.
2. Correct generator semantics required by qualification banks, including attenuation, tuning, envelopes, loop modes, filter/pan/effect sends and sample addressing.
3. SF2 default modulators needed for normal MIDI behavior.
4. `imod` / `pmod` support sufficient for real-world banks such as GeneralUser GS and TimGM6mb.
5. Stereo-linked sample support.
6. Broader SF2 2.x features only after profiling on N16R8.

Unsupported features must fail gracefully or be reported clearly; do not silently claim compatibility.

A compatibility analyzer should report why a user SF2 may not render accurately.

## Verification

Host/cloud verification should include:
- builds
- unit tests
- SF2 parsing tests
- zone-resolution tests
- generator/modulator tests
- MIDI state tests
- deterministic DSP tests where practical
- compatibility reports against public reference banks
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
