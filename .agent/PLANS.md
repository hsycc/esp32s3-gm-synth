# Implementation Plan

This file is the durable execution plan for Codex/local development. Keep it aligned with the implementation and measured evidence; do not leave completed milestones described as future work.

## Current project position

The project is currently in **M6 physical product qualification**.

M1 through the core M5 runtime path are established. Physical N16R8 storage, Program Prepare, Voice DSP, FX, I2S, ES8311, PA enable and audible speaker output have already been exercised. The active work is now:

1. separate audible-correctness validation from deliberately harsh synthetic performance stress;
2. measure stable normal/full-FX polyphony;
3. measure stable deliberately heavy-DSP polyphony;
4. run the extended physical I2S/music timeline stress;
5. choose a conservative product polyphony specification;
6. then move into M7 real-bank/GM128 release qualification.

The checkpoint before this plan update was `73a4765` (`test: complete physical suite after heavy DSP failure`).

## Fixed product baseline

- ESP32-S3 N16R8
- 16 MB flash
- 8 MB Octal PSRAM
- ESP-IDF v6.1
- 44.1 kHz / 16-bit audio
- stereo synth/mix path
- current Korvo V4 qualification speaker path is mono downmix through one ES8311 DAC / PA / speaker
- **16 MIDI channels / 16-part multitimbral GM runtime**
- GM1 128 melodic programs + drums
- normal firmware PCM residency arena default: 4 MiB
- current dedicated physical qualification arena candidate: 4.50 MiB
- local hardware keys may initially emit a fixed or small discrete set of velocities
- engine MIDI velocity compatibility remains 1..127

### Terminology that must stay unambiguous

- **MIDI channel count**: 16. Internal indices are 0..15. GM Channel 10 is index 9.
- **Polyphony / active voices**: simultaneous sample voices. This is independent of MIDI channel count.
- A single MIDI channel may consume multiple sample voices because of chords, release tails and layered SF2 zones.
- 16 MIDI channels therefore does not mean 16-voice or 32-voice polyphony.

R16 is excluded. SoundFont file size is not itself the synth-engine limit.

## Product-format decision

Native `.sf2` is a first-class user asset format.

Users should be able to create/share a SoundFont with existing tools and import the same `.sf2` into the product. A private compiled bank format is not required as the user-facing path.

Firmware may generate disposable indexes/caches from an SF2, but the original `.sf2` remains the portable asset.

## Public compatibility baselines

Use multiple banks because no single SoundFont is the specification:

- FM/GM Open SoundFont — simple/open GM128 baseline
- GeneralUser GS — real-world generator/modulator baseline
- FluidR3_GM — larger classic GM / partial-residency stress baseline
- TimGM6mb — compact real-world N16R8/modulator-heavy baseline
- GMBK5X128 — non-standard robustness/stress input only

## Validation layers

Do not collapse different evidence classes into one claim:

1. **Host tests** — deterministic SF2 semantics, MIDI/channel behavior, ownership/state machines, DSP/runtime logic.
2. **ESP32-S3 QEMU** — Xtensa + ESP-IDF + FreeRTOS + VFS/FATFS + PSRAM integration and panic paths.
3. **Physical N16R8** — actual memory reserve, Flash/optional-SD timing, DSP CPU cost, I2S deadlines, codec/PA/speaker behavior and stable polyphony.

QEMU throughput/latency is functional telemetry only and is never physical performance evidence.

Physical performance stress and audible-quality validation are also separate evidence classes: a synthetic worst-case fixture may intentionally sound harsh and must not be used as proof that normal SF2 playback sounds correct.

## M0 — N16R8 hardware baseline

Status: **established for the current qualification board.**

Established:
- ESP-IDF v6.1 pinned in CI
- ESP32-S3 N16R8 build assertions
- 16 MiB Flash / 8 MiB Octal-PSRAM configuration
- ESP32-S3 QEMU smoke path with explicit 8 MiB Octal PSRAM
- physical qualification path on KS_S3_KORVO_V4 / N16R8-class ESP32-S3
- physical boot detects 8 MiB Octal PSRAM at 80 MHz
- internal SRAM / PSRAM telemetry around residency allocation
- physical internal-Flash -> PSRAM benchmark
- physical 44.1 kHz I2S clock/DMA path
- ES8311 control/readback
- PA enable
- audible speaker output

Measured physical internal-Flash -> PSRAM throughput is approximately 5.8–6.3 MiB/s across the 4/8/16/32/64 KiB sweep. The 32 KiB source chunk remains the practical baseline because 64 KiB gains little aggregate throughput while increasing per-read tail latency.

Exit criteria:
- reproducible ESP32-S3 build: **met**
- known qualification configuration/board: **met for current Korvo V4 path**
- measured physical memory/storage baseline: **met**
- physical audio/DMA baseline: **met**

## M1 — SF2 analyzer

Status: **complete enough for current product work and CI-backed.**

Implemented analysis includes:
- RIFF/SF2 structure
- source identity/hash and file size
- `smpl` payload
- preset/instrument/sample counts
- bank distribution
- sample-rate distribution
- generator/modulator usage
- linked/stereo samples
- per-program dependencies/working sets
- realistic 16-channel sample unions
- N16R8 residency estimates

## M1.5 — SF2 compatibility baseline

Status: **core compatibility reporting established.**

Coverage includes:
- generator/modulator feature classification
- unsupported/malformed feature reporting
- stereo-link/sm24/ROM presence reporting
- real-bank qualification roles

Parseability remains separate from audio-runtime qualification.

## M2 — Generator hierarchy and ResolvedZone

Status: **implemented and CI-backed.**

Flow:

`Preset global/local -> Instrument global/local -> Sample -> ResolvedZone`

Implemented/tested semantics include:
- preset/instrument global/local range intersection
- instrument local generator overrides
- preset generator contribution rules
- sample address/loop offsets
- root key/tuning
- envelopes/filter/pan/attenuation/FX sends in native SF2 units

Representative results are cross-checked through the FluidSynth oracle used by M2.5.

## M2.5 — Modulator resolver / runtime modulation

Status: **core resolver established; major dynamic modulation paths are integrated.**

Pinned reference baseline:
- FluidSynth 2.6.1
- commit `71e85b2ca6bf48641ba7e2261d8f2640ae473773`
- `fluid_voice_gen_value()` effective-generator oracle

Established runtime coverage includes:
- generator hierarchy
- attenuation/pan
- filter/resonance
- volume envelope
- modulation envelope
- vibrato LFO
- modulation LFO
- pitch bend/range
- modulation wheel
- channel pressure
- CC91/CC93 FX sends

Runtime compatibility quirks remain separate from native parsed SF2 data.

## M3 — Sample residency / storage / Program Prepare

Status: **core architecture, QEMU integration and current physical N16R8 internal-Flash/Program-Prepare qualification are complete. SD comparison is optional product-architecture work, not an M4 blocker.**

Implemented path:

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

Implemented/tested capabilities include:
- fixed-capacity contiguous arena allocator
- aligned first-fit + free-span coalescing
- duplicate SampleId reuse
- Program Pin / Voice Ref lifetime separation
- generation/lease protection
- high-water/fragmentation telemetry
- transactional direct-to-arena loading and rollback
- no sample-sized staging requirement
- cross-core publication and deferred reclamation
- READY/Active snapshot exchange
- release-tail survival through Program Change
- resident-cache reuse
- safe LRU eviction
- explicit non-destructive capacity failure
- **16-channel** latest-wins Program Change mailbox
- real-SF2 VFS -> PSRAM QEMU integration
- physical cold/warm Program Prepare telemetry
- physical cache-pressure / eviction / commit / retire telemetry

The dedicated physical qualification image currently uses a 4.50 MiB arena candidate because the representative 16-channel Active + worst READY transaction exceeded the normal 4 MiB production default in host qualification.

V1 admission rule:
- evict only safe unowned cache;
- never destroy Active/READY Pins or live Voice Refs to force a new Program Change to fit;
- return/defer explicit capacity failure while the previous Active Program remains valid;
- streaming/degraded playback is a separate future feature.

Realtime invariant: no filesystem/Flash/SD read in the audio hot path.

## M4 — Voice DSP + FX + physical audio

Status: **implemented and physically exercised; no longer an unstarted milestone.**

Established paths include:
- sample interpolation / phase stepping
- pitch/sample-rate conversion
- looping
- attenuation / pan
- volume envelope
- resonant low-pass filter
- dynamic filter control
- vibrato LFO
- modulation LFO
- modulation envelope
- reverb/chorus sends
- global reverb / chorus
- bounded block render telemetry
- 44.1 kHz I2S DMA
- ES8311 DAC startup/readback
- PA enable
- actual speaker output

### M4 audible-correctness gate — required before M6 product sign-off

The current physical performance fixtures are intentionally pathological and can sound noisy/harsh:
- polyphony stress can start 16/20/24/28/32 copies of the same sample at differing pitches;
- music stress deliberately creates dense chromatic bursts, percussion, sustain, pitch bend, pressure and FX;
- heavy-DSP stress deliberately combines resonant filtering, dynamic filtering, LFOs, modulation envelope and full FX.

Therefore add/retain a separate **clean audible sanity path** that is not a performance stress test:
- physical reference tone must be clean and deterministic;
- use a real SF2 melodic Program for synth sanity, not an arbitrary synthetic stress sample;
- test 1 Voice first, then a small musical chord;
- begin dry/no-FX, then enable individual DSP features one at a time;
- verify attack/loop/release and pitch audibly;
- report PCM peak/clipping and checksum for the clean path;
- only after this clean gate passes should noisy synthetic stress results be interpreted solely as timing/polyphony evidence.

If the hardware-isolation reference tone itself is distorted/noisy, investigate I2S frame/slot format, DAC/PA level and physical output before blaming SF2/DSP.

## M5 — Timestamped realtime event scheduler / GM channel runtime

Status: **core runtime is implemented and CI/stress-backed.**

Path:

`input/event producer -> timestamp -> bounded queue -> block scheduler -> frame-offset execution`

Established behavior includes:
- bounded realtime event queue
- intra-block timestamped execution
- Note On/Off
- Control Change
- Sustain
- Pitch Bend / pitch range
- Channel Pressure
- Program Change handoff
- GM Reset
- 16 independent MIDI channel state slots
- GM Channel 10 percussion policy
- latest-wins Program Change control handoff

M5 exit criteria are effectively met for the current runtime; M7 will expand real input/user-bank qualification.

## M6 — Adaptive polyphony / physical product qualification

Status: **active milestone.**

Implemented/available:
- render deadline telemetry
- min/avg/P95/P99/max timing
- clipping counters
- adaptive-polyphony/deadline infrastructure
- safe voice management/stealing infrastructure
- physical normal/full-FX polyphony sweep
- physical deliberately heavy-DSP sweep
- long physical I2S/DMA stress image
- music/event timeline stress

Current physical normal/full-FX work has already demonstrated a 28-voice stable result in an earlier checkpoint, but this is not yet the public product polyphony specification.

The heavy-DSP sweep now measures:

`1 -> 2 -> 4 -> 8 -> 12 -> 16 -> 20 -> 24 -> 28 -> 32`

and records active voices, deadline misses, clipping and fixture failures so a failure at 8 voices can no longer be misreported as an unexplained `heavy_dsp_max=0`.

The current heavy fixture deliberately enables:
- resonant low-pass
- dynamic low-pass control
- vibrato LFO
- Mod LFO -> pitch/filter/volume
- Mod Envelope -> pitch/filter
- chorus
- reverb

The extended physical I2S measurement window is 16384 blocks at 64 frames / 44.1 kHz, about 23.78 seconds.

### M6 sign-off sequence

1. **Clean audible gate passes.**
2. Normal/full-FX physical polyphony sweep produces a stable maximum.
3. Heavy-DSP physical sweep produces a stable maximum with a valid fixture.
4. Extended I2S/music stress has zero unacceptable DMA/write/deadline failures.
5. No clipping in qualification cases intended to be unclipped.
6. Select:

```text
measured_candidate = min(full_fx_max, heavy_dsp_max)
```

7. Apply engineering margin; the public product spec must be lower/equal to the measured candidate, not simply the highest voice count ever observed.

The original qualification goal remains at least 16 active sample voices. Higher public counts require measured margin across real SF2 workloads, not only a synthetic fast path.

## M7 — GM128 / user-SF2 release qualification

Status: **partial CI coverage exists; full product qualification follows M6 sign-off.**

Functional coverage target:
- 128 GM melodic programs and drums where provided
- **16 MIDI channels**
- Program/Bank change
- Note On/Off
- sustain
- volume/pan/expression
- pitch bend/range
- reverb/chorus sends
- default/custom modulators
- stereo-linked samples

Stress coverage target:
- dense chords/sustain
- drum bursts
- rapid Program Change
- mixed 16-channel working sets
- maximum-zone/layer programs
- long-running playback
- multiple representative public SF2 banks

Release metrics:
- deadline misses / underruns / queue drops
- invalid sample accesses
- max/P95/P99 render time
- internal SRAM/PSRAM high-water marks
- residency pressure
- source load/reuse/eviction bytes and latency
- observed stable polyphony by DSP complexity class
- clean audible correctness on representative melodic and percussion content

## Current decision log

- Hardware target is fixed: ESP32-S3 N16R8; R16 assumptions are prohibited.
- ESP-IDF v6.1 is the pinned embedded baseline.
- Native SF2 is the user-facing/shareable asset format.
- V1 GM runtime is **16 MIDI channels**, not 32.
- MIDI channel count and simultaneous voice/polyphony count are independent concepts.
- Shared/per-program residency is mandatory architecture; full residency is only an optimization.
- SF3 is not a V1 priority.
- Private compiled-bank conversion is not required for user assets; disposable indexes/caches are allowed.
- Native SF2 sample loading is random-access/chunked and control-domain only.
- Residency admission never destroys Active/READY/live-Voice ownership merely to make a Program Change fit.
- Rapid Program Changes use latest-wins coalescing at compact Program identity level.
- QEMU is an integration gate, not a physical performance oracle.
- Internal Flash was benchmarked physically before optional SD work.
- Synthetic performance stress is not an audible-quality oracle.
- Product polyphony will be chosen from measured normal/full-FX and heavy-DSP limits with engineering margin.
