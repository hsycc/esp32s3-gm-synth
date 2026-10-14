# Implementation Plan

This file is the durable execution plan for Codex/local development. Keep it updated as milestones are completed or materially redesigned.

## Fixed product baseline

- ESP32-S3 N16R8
- 16 MB flash
- 8 MB PSRAM
- 44.1 kHz / 16-bit / stereo
- 16 MIDI channels
- GM1 128 melodic programs + drums

R16 is excluded. SoundFont file size is not a product requirement.

## M0 — Baseline and measurement harness

Goal: establish a trustworthy starting point before major refactors.

Tasks:
- choose the initial ESP-IDF/Arduino integration strategy
- select/record the actual N16R8 development board/module
- reproduce the relevant behavior of the SF2 reference implementation
- establish 44.1 kHz stereo I2S output
- add block render-time telemetry
- record internal SRAM and PSRAM usage
- test 8 / 12 / 16 / 19 active voices where possible
- create a repeatable dense-MIDI stress sequence

Exit criteria:
- reproducible build
- known N16R8 hardware target/configuration
- measured baseline for CPU/block timing and memory
- measured safe PSRAM reserve, not a guessed reserve
- no untracked assumptions about maximum polyphony

## M1 — SF2 analyzer

Goal: understand real candidate GM SoundFonts before designing around file size alone.

Analyzer reports:
- RIFF/SF2 structure
- source identity/hash and file size
- `smpl` PCM payload size
- preset/instrument/sample counts
- bank/preset distribution
- source sample-rate distribution
- sample alignment/residency estimate
- program -> sample dependency sets
- per-program PCM working set
- per-program zone count
- maximum overlapping zones for representative notes/velocities
- drum-bank requirements
- used generator opcodes
- used preset/instrument modulators with amount classification
- linked/stereo sample usage
- machine-readable JSON report

Current real profiles:
- `TimGM6mb.sf2`: compact, full-residency candidate, modulator-heavy
- `GMBK5X128.sf2`: large multi-bank source, requires offline pruning + shared/per-program residency, simpler modulator semantics

See `tasks/M1_SF2_ANALYZER.md` and `docs/SF2_BANK_ANALYSIS.md`.

Exit criteria:
- analyzer can process both current candidate banks
- N16/R8 persistent/resident feasibility can be answered with evidence
- required SF2 feature subsets are documented
- product asset compiler/pruner requirements are defined

## M2 — Engine decomposition

Goal: separate responsibilities while preserving behavior.

Target domains:
- `gm/` — GM channel semantics and reset/bank/program state
- `sf2/` — source parser and/or compiled zone representation
- `sample/` — shared residency, handles, references and future stream abstraction
- `synth/` — voice allocation and voice state
- `audio/` — block rendering and I2S
- `dsp/` — envelope/filter/interpolation/effects
- `storage/` — compiled bank reads and background asset preparation
- `diagnostics/` — counters and timing

Rules:
- structural changes first
- do not change sound/DSP intentionally during decomposition unless required to correct a documented SF2 semantic gap
- add tests around behavior before moving code
- do not design the runtime around raw source-SF2 traversal if the offline compiler can precompute it safely

## M3 — Timestamped event scheduler

Goal: adopt a Phoenix-style musical event path suitable for MIDI and future internal sequencing.

Target flow:

`MIDI/other input -> decoder -> timestamp -> bounded queue -> block scheduler -> frame-offset execution`

Tasks:
- define compact event structure
- implement bounded realtime-safe queue
- convert timestamps to audio-frame offsets
- support multiple events inside one audio block
- measure queue pressure and scheduling misses

Exit criteria:
- events can execute at distinct frame offsets inside a block
- audio task never blocks waiting for input/control tasks

## M4 — Adaptive polyphony and safe voice stealing

Goal: prefer graceful degradation over deadline misses/clicks.

Tasks:
- block deadline monitor
- rolling/peak render-time counters
- CPU-pressure signal
- adaptive voice-admission limit
- hysteresis/recovery policy
- fast-release/fade voice stealing
- duplicate-note and exclusive-class policy

Exit criteria:
- load spikes reduce admission before repeated deadline failure
- polyphony recovers gradually after sustained stability
- stealing avoids obvious hard discontinuities under stress

## M5 — Asset compiler, residency and Program Change

Goal: deploy product-specific assets in N16 and remove synchronous asset loading from realtime audio transitions.

Required capabilities:
- host-side GM asset compiler/pruner
- eliminate unexposed banks/presets and unreferenced samples
- precompute program -> sample dependency sets
- shared/per-program PSRAM residency
- optional full-resident fast path for compact compiled banks
- prepare / ready / block-boundary commit / safe retire lifecycle

Optional experiments:
- 48 kHz -> 44.1 kHz offline resampling for banks such as GMBK5X128, only after sound-quality evaluation
- additional metadata flattening/precomputation to reduce runtime parser/control cost

Exit criteria:
- compiled product bank fits the N16 partition plan
- no filesystem/flash reads in audio hot path
- realistic 16-channel active sample unions fit R8 with measured reserve
- Program Change behavior is measured and documented
- old sound can continue until new program is ready where practical

## M6 — GM128 qualification

Goal: turn “it plays” into a release-quality validation matrix.

Functional coverage:
- 128 melodic programs
- selected channel-10 drum profile(s)
- 16 MIDI channels
- Note On/Off
- Program Change
- Bank Select
- pitch bend and bend range
- CC7 volume
- CC10 pan
- CC11 expression
- CC64 sustain
- CC91 reverb send
- CC93 chorus send
- GM reset

Stress coverage:
- dense piano chords
- long sustain
- repeated same-note retrigger
- drum bursts
- rapid Program Change
- 16-channel MIDI mixes
- maximum-zone/layer presets
- long-running playback

Release metrics must include:
- deadline misses
- underruns
- queue drops
- invalid sample accesses
- max and percentile render time
- memory high-water marks
- sample-residency pressure/high-water mark
- observed stable polyphony

## Current decision log

- Hardware target is fixed: ESP32-S3 N16R8; do not use R16 assumptions.
- Source `.sf2` file size is not a target metric; compiled persistent footprint and active PCM union are.
- Primary product direction: validated GM profile(s), not arbitrary-SF2 compatibility first.
- Shared/per-program residency is a required architecture path.
- Full residency is an optional optimization for compact banks such as TimGM6mb if N16R8 measurements prove safe reserve.
- Large multi-bank sources such as GMBK5X128 must be treated as compiler input, not automatically as deployable raw assets.
- Streaming remains an architectural extension rather than a current V1 requirement.
- Guaranteed polyphony target: 16 voices; higher values are performance targets only until hardware validation.
- Audio block baseline: 64 frames at 44.1 kHz.
- Reference project ideas may be combined; none is treated as the final architecture.
