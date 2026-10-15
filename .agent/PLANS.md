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

Status: hardware/toolchain integration still pending; Host semantic work is intentionally progressing in parallel.

Goal: establish a trustworthy hardware/build starting point before performance claims.

Tasks:
- choose and pin ESP-IDF integration/toolchain strategy
- select/record actual N16R8 board/module
- establish 44.1 kHz stereo I2S output
- add block render-time telemetry
- record internal SRAM and PSRAM usage
- create repeatable dense-MIDI stress sequences

Exit criteria:
- reproducible ESP32-S3 build
- known N16R8 hardware target/configuration
- measured memory reserve and audio deadline baseline

## M1 — SF2 analyzer

Status: useful real-bank implementation complete and used by later CI.

Implemented analysis includes:
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

Its checked-in manifests now generate real dependency fixtures for M3 Host tests.

## M1.5 — SF2 compatibility baseline

Status: core compatibility reporting exists; qualification expands as DSP/runtime support expands.

Coverage includes:
- generator/modulator feature classification
- unsupported/malformed feature reporting
- stereo-link/sm24/ROM presence reporting
- real-bank qualification roles

Parseability remains explicitly separate from audio-runtime qualification.

## M2 — Generator hierarchy and ResolvedZone

Status: Host hierarchy/ResolvedZone implementation established and used by conformance tooling.

Target flow:

`Preset global/local -> Instrument global/local -> Sample -> ResolvedZone`

Implemented/tested semantics include:
- preset/instrument global/local range intersection
- instrument local generator overrides
- preset generator contribution rules
- sample address/loop offsets
- root key/tuning
- envelopes/filter/pan/attenuation/FX sends in native SF2 units

Representative results are cross-checked through the M2.5 FluidSynth oracle.

## M2.5 — Modulator resolver

Status: **L0/L1 complete and strict in CI.**

Pinned reference:
- FluidSynth 2.6.1
- commit `71e85b2ca6bf48641ba7e2261d8f2640ae473773`
- internal `fluid_voice_gen_value()` effective-generator oracle

Strict real-bank baseline:
- FM/GM Open subset
- GeneralUser GS subset
- FluidR3_GM subset
- programs 8/16/32/50/56/80/88/96
- key 60 / velocity 96

Current strict result:
- structural failures: 0
- generator failures: 0
- core Generator mismatch count: 0

Runtime compatibility behavior such as FluidSynth/EMU attenuation scaling and the default Velocity->FilterFc compatibility quirk is kept separate from native parsed SF2 data.

L2 dynamic CC/pitch/pressure expansion may continue in parallel with later runtime milestones.

## M3 — Sample residency manager

Status: **Host architecture largely implemented and CI-backed. Hardware backend remains.**

Goal: make large user SF2 files practical on R8 without storage I/O from NoteOn/audio render.

### Implemented Host path

```text
native .sf2
  -> RandomAccessReader
  -> Sf2SampleSource
  -> Program dependency set
  -> ProgramPrepareController
  -> direct-to-arena chunked loading
  -> Program Pins
  -> PublishedSampleDirectory
  -> immutable READY ProgramSnapshot
  -> block-boundary Commit
  -> move-only Voice Ref
```

Implemented/tested capabilities:
- fixed-capacity contiguous arena allocator
- aligned first-fit + free-span coalescing
- duplicate SampleId reuse
- independent Program Pin / Voice Ref lifetimes
- generation/lease protection
- high-water/fragmentation telemetry
- direct-to-arena transactional source loading with rollback after partial read failure
- no sample-sized staging buffer requirement
- cross-core publication protocol with acquire grace period and deferred reclamation
- block-boundary READY/Active snapshot exchange
- old release tails survive Program Change
- source-backed transactional Prepare with resident-cache reuse
- generated real-bank 16-channel dependency stress
- native SF2 `smpl`/`shdr` random-access source validated against real files
- safe LRU eviction of unowned/unpublished/reclaimable cache entries
- explicit residency-capacity failure that preserves the previous Active Program
- compact 16-channel latest-wins Program Change mailbox for rapid UI/MIDI changes

### Measured residency pressure

Checked-in qualification manifests currently show:
- FM/GM subset greedy 16-program union: `2,221,452` B
- GeneralUser subset greedy 16-program union: `5,675,820` B
- FluidR3 subset greedy 16-program union: `5,887,528` B

Therefore "only keep current 16 Programs" is not by itself a guarantee that a user SF2 fits R8. Arbitrary active working sets can exceed the available PCM arena.

### Admission policy

V1 rule:
- Prepare may evict only safe unowned cache;
- Active/READY Pins and live Voice Refs are never sacrificed to force a new Program Change to fit;
- if the requested working set cannot coexist during safe Prepare, return a capacity failure/defer the change while the old Active Program remains valid;
- streaming/degraded playback is a separate future feature, not a hidden fallback.

### M3 remaining

Host/control:
- integrate compact latest-desired mailbox with the eventual program dependency resolver/control loop
- add prepare latency/source byte/eviction diagnostics where useful
- continue pathological fragmentation/capacity tests

ESP32-S3 Phase C:
- allocate arena from PSRAM (`MALLOC_CAP_SPIRAM` or selected abstraction)
- implement Flash/LittleFS `RandomAccessReader`
- optionally add SD/MMC backend later
- keep hot publication/snapshot metadata in internal RAM where measurements justify it
- verify storage calls never run on audio task
- measure PSRAM high-water/load latency
- measure atomic acquire/release and commit cost on actual S3

Exit criteria:
- large native SF2 can select playable presets using only required resident unions when the requested working set fits
- capacity failure is explicit and non-destructive
- active/releasing voices retain valid PCM through Program Changes
- no filesystem/flash read occurs in the audio hot path
- N16R8 hardware measurements validate the chosen memory reserve and realtime costs

## M4 — Voice DSP baseline

Goal: make ResolvedZone + ModPlan + SampleRef produce sound correctly before aggressive optimization.

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

Entry condition:
- M3 sample lifetime/publication contract remains stable enough that Voice can own a `PublishedVoiceRef` without storage knowledge.

Exit criteria:
- representative reference notes render correctly enough for semantic A/B comparison
- deterministic DSP tests exist where practical

## M5 — Timestamped realtime event scheduler

Goal: move MIDI/internal events into a bounded realtime-safe path.

`input -> MIDI/internal decoder -> timestamp -> bounded queue -> block scheduler -> frame-offset execution`

Exit criteria:
- multiple Note/CC events can execute at distinct offsets within a 64-frame block
- audio task never blocks on producers

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
- source load/reuse/eviction bytes and latency
- observed stable polyphony by DSP complexity class

## Current decision log

- Hardware target is fixed: ESP32-S3 N16R8; R16 assumptions are prohibited.
- Native SF2 is the user-facing/shareable asset format.
- Local keys may use a fixed or small discrete set of velocities; engine compatibility remains 1..127.
- Shared/per-program residency is mandatory architecture; full residency is only an optimization.
- SF3 is not a V1 priority.
- Private compiled-bank conversion is not required for user assets; disposable indexes/caches are allowed.
- Modulators are resolved outside the per-sample interpretation path; continuous DSP cost, not arbitrary table scanning, is the main polyphony risk.
- Native SF2 sample loading is random-access/chunked and control-domain only; no full-file or full-sample staging copy is required.
- Residency admission never destroys Active/READY/live-Voice ownership merely to make a new Program Change fit.
- Rapid Program Changes use latest-wins coalescing at the compact Program identity level; dependency sets are recomputed when the request is actually serviced.
- 16 active sample voices remains the qualification goal; higher counts depend on measured N16R8 headroom.
