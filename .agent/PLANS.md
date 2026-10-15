# Implementation Plan

This file is the durable execution plan for Codex/local development. Keep it updated as milestones are completed or materially redesigned.

## Fixed product baseline

- ESP32-S3 N16R8
- 16 MB flash
- 8 MB Octal PSRAM
- ESP-IDF v6.1
- 44.1 kHz / 16-bit / stereo
- 16 MIDI channels
- GM1 128 melodic programs + drums
- initial PCM residency arena: 4 MiB
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

## Validation layers

Do not collapse different evidence classes into one claim:

1. Host tests validate deterministic semantics and ownership/state-machine behavior.
2. ESP32-S3 QEMU validates Xtensa + ESP-IDF + FreeRTOS + VFS/FATFS + PSRAM integration and panic paths.
3. Physical N16R8 validates actual memory reserve, Flash/SD timing, DSP cost, I2S deadlines and stable polyphony.

QEMU throughput/latency numbers are functional telemetry only and never hardware performance evidence.

## M0 — N16R8 hardware baseline

Status: **reproducible ESP-IDF v6.1 / ESP32-S3 build and 8 MiB Octal-PSRAM QEMU baseline complete; physical N16R8 measurements and audio baseline remain.**

Established:
- ESP-IDF v6.1 pinned in CI
- ESP32-S3 N16R8 build assertions
- 16 MiB Flash / Octal PSRAM configuration
- 4 MiB PSRAM arena bring-up path
- ESP32-S3 QEMU smoke test with explicit 8 MiB Octal PSRAM
- physical-board bring-up checklist and dedicated benchmark image build path

Remaining physical tasks:
- select/record exact N16R8 module/board SKU used for qualification
- record clean-boot internal SRAM/PSRAM reserve
- establish 44.1 kHz stereo I2S output in M4
- add block render-time telemetry with the audio engine
- create/run repeatable dense-MIDI stress sequences

Exit criteria:
- reproducible ESP32-S3 build: **met**
- known N16R8 target/configuration: **configuration met; exact qualification board record pending**
- measured physical memory reserve: pending
- measured audio deadline baseline: pending M4+

## M1 — SF2 analyzer

Status: **useful real-bank implementation complete and used by later CI.**

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

Its checked-in manifests generate real dependency fixtures for M3 Host tests.

## M1.5 — SF2 compatibility baseline

Status: **core compatibility reporting established; qualification expands as DSP/runtime support expands.**

Coverage includes:
- generator/modulator feature classification
- unsupported/malformed feature reporting
- stereo-link/sm24/ROM presence reporting
- real-bank qualification roles

Parseability remains explicitly separate from audio-runtime qualification.

## M2 — Generator hierarchy and ResolvedZone

Status: **Host hierarchy/ResolvedZone implementation established and used by conformance tooling.**

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

## M3 — Sample residency / storage

Status: **Host architecture + ESP-IDF/VFS backend + real-SF2 ESP32-S3 QEMU integration are complete and CI-backed. Physical N16R8 performance qualification remains.**

Goal: make large user SF2 files practical on R8 without storage I/O from NoteOn/audio render.

### Implemented path

```text
native .sf2
  -> VFS RandomAccessReader
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
- ESP-IDF `VfsRandomAccessReader`
- ESP32-S3 PSRAM arena allocation abstraction
- storage benchmark core with 4/8/16/32/64 KiB sweep
- throughput plus min/avg/p50/p95/p99/max latency telemetry
- generated raw-FATFS real-SF2 fixture
- ESP32-S3 QEMU integration using 16 MiB virtual Flash and exactly 8 MiB Octal PSRAM
- real SF2 opens/parses from `/sf2/test.sf2` in QEMU as 67 playable samples
- QEMU real-SF2 VFS -> PSRAM sweep passes all five chunk sizes
- checked benchmark-log parser that emits a Markdown PASS/FAIL report
- dedicated physical N16R8 internal-Flash benchmark build using 4 MiB per chunk-size pass

### QEMU functional gate

First complete green real-SF2 run:
- workflow: `esp-idf-build #21`
- commit: `86e492b`
- fixture: `FM_GM_Open_subset_6MiB.sf2`
- fixture bytes: `5,898,214`
- fixture playable samples: `67`
- virtual PSRAM: `8,388,608` bytes
- PCM arena: `4,194,304` bytes
- QEMU benchmark bytes/pass: `262,144`
- all five chunk sizes passed

See `benchmarks/QEMU_SF2_STORAGE_BASELINE_2026-10-15.md`.

The observed QEMU ~2.3–2.4 MiB/s is not a physical performance result.

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

Physical N16R8 gate:
- boot the exact qualification board and record PSRAM total/free/largest/minimum-free values
- run the pinned internal-Flash -> PSRAM benchmark at 4 MiB per 4/8/16/32/64 KiB pass
- capture throughput and p95/p99/max latency with `tools.storage_benchmark.report`
- use the first physical result to define evidence-based performance acceptance thresholds
- decide whether the 4 MiB arena remains the production baseline or can safely move toward 5–6 MiB

Then, where product architecture requires it:
- add/mount SD/MMC backend and repeat the same real-SF2 benchmark
- compare Flash vs SD average throughput and tail latency
- measure Program Prepare source bytes/reuse/eviction latency on target
- measure atomic acquire/release and block-boundary commit cost on actual S3
- integrate latest-desired Program mailbox with the final control/dependency loop
- continue pathological fragmentation/capacity tests where useful

Realtime invariant to verify once M4 audio task exists:
- no filesystem/Flash/SD read occurs in the audio hot path.

Exit criteria:
- large native SF2 can select playable presets using only required resident unions when the requested working set fits
- capacity failure is explicit and non-destructive
- active/releasing voices retain valid PCM through Program Changes
- no filesystem/Flash read occurs in the audio hot path
- physical N16R8 memory/storage measurements validate the chosen reserve and control-domain load costs

## M4 — Voice DSP baseline

Status: **not started as the active integration milestone; entry waits for the physical M3 storage/memory baseline.**

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
- 44.1 kHz / 16-bit stereo I2S output
- bounded per-block render timing and underrun telemetry

Entry condition:
- M3 sample lifetime/publication contract remains stable enough that Voice can own a `PublishedVoiceRef` without storage knowledge;
- physical N16R8 PSRAM/Flash baseline is captured so storage faults are separated from DSP timing faults.

Exit criteria:
- representative reference notes render correctly enough for semantic A/B comparison
- deterministic DSP tests exist where practical
- first physical render/deadline baseline is recorded

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
- ESP-IDF v6.1 is the pinned embedded baseline.
- Native SF2 is the user-facing/shareable asset format.
- Local keys may use a fixed or small discrete set of velocities; engine compatibility remains 1..127.
- Shared/per-program residency is mandatory architecture; full residency is only an optimization.
- SF3 is not a V1 priority.
- Private compiled-bank conversion is not required for user assets; disposable indexes/caches are allowed.
- Modulators are resolved outside the per-sample interpretation path; continuous DSP cost, not arbitrary table scanning, is the main polyphony risk.
- Native SF2 sample loading is random-access/chunked and control-domain only; no full-file or full-sample staging copy is required.
- Residency admission never destroys Active/READY/live-Voice ownership merely to make a new Program Change fit.
- Rapid Program Changes use latest-wins coalescing at the compact Program identity level; dependency sets are recomputed when the request is actually serviced.
- QEMU is an integration gate, not a physical performance oracle.
- Internal Flash is benchmarked on physical N16R8 before adding SD so storage variables remain separable.
- No numeric Flash-throughput pass threshold is invented before the first physical measurement; the first board run establishes the evidence baseline.
- 16 active sample voices remains the qualification goal; higher counts depend on measured N16R8 headroom.
