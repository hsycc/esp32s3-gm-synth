# Implementation Plan

This file is the durable execution plan for Codex/local development. Keep it updated as milestones are completed or materially redesigned.

## Fixed product baseline

- ESP32-S3 N16R8
- 16 MB flash
- 8 MB PSRAM
- 44.1 kHz / 16-bit / stereo
- 16 MIDI channels
- GM1 128 melodic programs + drums
- local hardware keys may initially emit one fixed velocity (for example 96 or 100)
- the synth engine still accepts full MIDI velocity 1..127 from external MIDI/files

R16 is excluded. SoundFont file size is not itself a synth-engine limit.

## Product-format decision

Native `.sf2` is a first-class user asset format.

Users should be able to create/share a SoundFont with existing tools and import the same `.sf2` into the product. A private compiled bank format is not the required user-facing path.

Firmware may generate a disposable sidecar index/cache from an SF2, but the original `.sf2` remains the portable asset.

## Public compatibility baselines

Use multiple banks because no single SoundFont is the specification:

- FM/GM Open SoundFont: simple/open GM128 baseline
- GeneralUser GS: real-world generator/modulator compliance baseline
- FluidR3_GM: large classic GM / partial-residency stress baseline
- TimGM6mb: compact real-world N16R8/modulator-heavy baseline
- GMBK5X128: known non-standard robustness/stress input only

Public full banks and deterministic ~6 MiB derived fixtures live under `assets/sf2/` when Git LFS policy permits.

## M0 — N16R8 hardware baseline

Goal: establish a trustworthy hardware/build starting point before performance claims.

Tasks:
- choose and pin ESP-IDF/Arduino integration strategy
- select/record actual N16R8 board/module
- establish 44.1 kHz stereo I2S output
- add block render-time telemetry
- record internal SRAM and PSRAM usage
- create repeatable dense-MIDI stress sequences

Exit criteria:
- reproducible build
- known N16R8 hardware target/configuration
- measured memory reserve and audio deadline baseline

## M1 — SF2 analyzer

Status: first useful version implemented.

Goal: turn bank/storage/feature questions into measured data.

Implemented/expected analysis includes:
- RIFF/SF2 structure
- source identity/hash and file size
- `smpl` payload
- preset/instrument/sample counts
- bank distribution
- source sample-rate distribution
- generator/modulator usage
- linked/stereo samples
- per-program dependencies/working sets
- realistic 16-channel sample unions
- N16R8 residency estimates

## M1.5 — SF2 compatibility baseline

Goal: say precisely which standard SF2 semantics are supported, planned or unsupported.

Tasks:
- compatibility matrix for generators/modulators/features
- import warnings for unsupported or malformed features
- classify default modulators, `imod`, `pmod`, stereo links, `sm24`, ROM samples, etc.
- use FM/GM Open, GeneralUser GS, FluidR3 and TimGM as different qualification roles

Exit criteria:
- importing a bank produces a deterministic compatibility report
- unsupported features are explicit rather than silently ignored

## M2 — Generator hierarchy and ResolvedZone

Goal: convert general SF2 preset/instrument structures into final sample-playing zones before firmware DSP depends on them.

Target flow:

`Preset global/local -> Instrument global/local -> Sample -> ResolvedZone`

Required semantics:
- preset/instrument global/local range intersection
- instrument local generator supersedes instrument global/default
- preset local supersedes preset global
- valid preset generators increment instrument/default generator values
- invalid preset-level generators are ignored/reported
- sample address/loop offsets
- root key/tuning
- envelopes/filter/pan/attenuation/FX sends in native SF2 units

Status: host `ResolvedZone` implementation and hierarchy tests started.

Exit criteria:
- deterministic host tests cover four-level hierarchy
- representative real banks resolve without invalid sample references
- selected results are cross-checked against a standards-oriented reference such as FluidSynth

## M2.5 — Modulator resolver

Goal: support mainstream SoundFont dynamic behavior without interpreting all modulators inside the per-sample hot loop.

Required semantics:
- default SF2 modulators
- instrument global/local `imod` identity/override rules
- preset global/local `pmod` identity/add rules
- zero-amount instrument modulators that intentionally disable defaults
- common source curves: linear/concave/convex/switch, polarity/direction
- primary and amount-source controls where qualification banks use them

Runtime strategy:
- key/velocity/static modulation: NoteOn/preparation rate
- CC/pitch/pressure modulation: event/control rate
- only actual continuous DSP (LFO/envelope/filter) remains active in render path

Exit criteria:
- GeneralUser GS/TimGM modulator cases produce documented expected parameter behavior
- no per-sample scan over arbitrary `pmod/imod` tables

## M3 — Sample residency manager

Goal: make large user SF2 files practical on R8 without storage I/O from NoteOn/audio render.

Capabilities:
- full-resident fast path for small banks that safely fit
- shared/per-program PSRAM residency as general path
- stable sample handles and voice references
- background/control-domain sample loading
- prepare -> READY -> block-boundary commit -> safe retire
- fragmentation/high-water diagnostics

Exit criteria:
- large source SF2 can select playable presets while only required sample union is resident
- active voices keep valid samples through release tails
- no filesystem/flash read in audio hot path

## M4 — Voice DSP baseline

Goal: make ResolvedZone produce sound correctly before aggressive optimization.

Implement/validate:
- sample interpolation
- pitch/sample-rate conversion
- volume ADSR
- loops
- attenuation/pan
- per-voice filter where required
- vibrato/mod LFO where required
- modulation envelope where required
- dry/reverb/chorus sends

Exit criteria:
- representative reference notes render correctly enough for semantic A/B comparison
- deterministic DSP tests exist where practical

## M5 — Timestamped realtime event scheduler

Goal: move MIDI/internal events into a bounded realtime-safe path.

`input -> decoder -> timestamp -> bounded queue -> block scheduler -> frame-offset execution`

Exit criteria:
- multiple events can execute at distinct offsets within a 64-frame block
- audio task never blocks on control/input work

## M6 — Adaptive polyphony and safe voice stealing

Goal: preserve audio deadlines across simple and DSP-heavy SF2 programs.

Tasks:
- render deadline monitor
- rolling/peak/P99 timing
- adaptive voice-admission limit
- hysteresis/recovery
- fast-release/fade stealing
- optional per-voice DSP complexity weight as a predictive signal

Qualification target remains 16 active sample voices; 18–20 remains opportunistic until N16R8 measurements prove margin.

## M7 — GM128 / user-SF2 qualification

Functional coverage:
- 128 GM melodic programs and drums where provided
- 16 MIDI channels
- Program/Bank change
- Note On/Off, sustain, volume, pan, expression
- pitch bend/range
- reverb/chorus sends
- default/custom modulators
- stereo-linked samples

Stress coverage:
- dense chords/sustain
- drum bursts
- rapid Program Change
- mixed 16-channel working sets
- maximum-zone/layer programs
- long-running playback

Release metrics:
- deadline misses / underruns / queue drops
- invalid sample accesses
- max/P95/P99 render time
- internal SRAM/PSRAM high-water marks
- sample residency pressure
- observed stable polyphony by DSP complexity class

## Current decision log

- Hardware target is fixed: ESP32-S3 N16R8; R16 assumptions are prohibited.
- Native SF2 is the user-facing/shareable asset format.
- Local keys may use a fixed or small discrete set of velocities; engine compatibility remains 1..127.
- Shared/per-program residency is mandatory architecture; full residency is only an optimization.
- SF3 is not a V1 priority.
- Private compiled-bank conversion is not required for user assets; disposable indexes/caches are allowed.
- Modulators must be resolved outside the per-sample interpretation path; continuous DSP cost, not table parsing, is the main polyphony risk.
- 16 active sample voices remains the qualification goal; higher counts depend on measured N16R8 headroom.
