# Implementation Plan

This file is the durable execution plan for Codex/local development. Keep it updated as milestones are completed or materially redesigned.

## M0 — Baseline and measurement harness

Goal: establish a trustworthy starting point before major refactors.

Tasks:
- choose the initial ESP-IDF/Arduino integration strategy
- reproduce the relevant behavior of the SF2 reference implementation
- establish 44.1 kHz stereo I2S output
- add block render-time telemetry
- record internal SRAM and PSRAM usage
- test 8 / 12 / 16 / 19 active voices where possible
- create a repeatable dense-MIDI stress sequence

Exit criteria:
- reproducible build
- known hardware target/configuration
- measured baseline for CPU/block timing and memory
- no untracked assumptions about maximum polyphony

## M1 — SF2 analyzer

Goal: understand the actual fixed GM SoundFont before designing around file size alone.

Create a host-side analyzer that reports:
- RIFF/SF2 structure
- file size
- `smpl` PCM payload size
- preset/instrument/sample counts
- sample alignment/residency estimate
- program -> sample dependency sets
- per-program PCM working set
- per-program zone count
- maximum overlapping zones for representative notes/velocities
- drum-bank requirements
- used generator opcodes
- used preset/instrument modulators
- linked/stereo sample usage
- a machine-readable JSON report

Exit criteria:
- analyzer can process the chosen GM bank
- full-residency feasibility on target PSRAM can be answered with evidence
- required SF2 feature subset is documented

## M2 — Engine decomposition

Goal: separate responsibilities while preserving behavior.

Target domains:
- `gm/` — GM channel semantics and reset/bank/program state
- `sf2/` — parser and zone resolution
- `sample/` — residency, handles, references and future stream abstraction
- `synth/` — voice allocation and voice state
- `audio/` — block rendering and I2S
- `dsp/` — envelope/filter/interpolation/effects
- `diagnostics/` — counters and timing

Rules:
- structural changes first
- do not change sound/DSP intentionally during decomposition
- add tests around behavior before moving code

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

## M5 — Asset residency and Program Change

Goal: remove synchronous asset loading from realtime audio transitions.

First decide based on M1:
- full fixed-bank residency if feasible, or
- per-program/shared-sample residency otherwise

Preferred transition model:
- prepare
- ready
- commit at block boundary
- retire safely

Exit criteria:
- no filesystem/flash reads in audio hot path
- Program Change behavior is measured and documented
- if loading is required, old sound can continue until new program is ready where practical

## M6 — GM128 qualification

Goal: turn “it plays” into a release-quality validation matrix.

Functional coverage:
- 128 melodic programs
- channel-10 drums
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
- observed stable polyphony

## Current decision log

- Primary product direction: fixed validated GM SoundFont, not arbitrary-SF2 compatibility first.
- Initial sample strategy: PSRAM residency, with streaming kept as an architectural extension rather than a V1 requirement.
- Guaranteed polyphony target: 16 voices; higher values are performance targets only until hardware validation.
- Audio block baseline: 64 frames at 44.1 kHz.
- Reference project ideas may be combined; none is treated as the final architecture.
