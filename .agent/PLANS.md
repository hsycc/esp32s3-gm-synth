# Implementation Plan

This is the durable execution plan for the ESP32-S3 GM / native-SF2 synth. Keep semantic/host evidence, QEMU integration evidence, physical timing evidence and subjective audible evidence separate.

## Current project position

The **M0–M8 source integration batch** is assembled on `agent/m6-sf2-playback-prepare-v1`. M7 PR #17 and M8 PR #18 have merged into that branch. Core implementation and tracked CI/physical stress work are complete for the tested configurations; representative audible approval and V1 release remain open.

M8 reference: `bb31908b` three physical runs PASS with PCM checksum `fe69915701738934`, 24 nominal normal/full-FX sample voices, eight stable pathological heavy-DSP voices, no WDT/hard-gate failures and final backlog zero. User confirmed the recorded `-dirty` suffix is not a blocker for this source integration; retain the actual banner without claiming clean CI binary identity. See `benchmarks/M8_PHYSICAL_THREE_RUN_2026-10-24.md`.

Current focus: integrate into `main` with CI, then complete audible tests, release DAC-level/headroom trials and final supported-asset packaging. These remain independent gates; no V1 publication is implied.

M6 physical product qualification is complete at:

`c16ff278b265018be9d2af5cc4fe3e54fe01e947`

Authoritative closeout:

`benchmarks/M6_PHYSICAL_QUALIFICATION_BASELINE_2026-10-20.md`

M6 established a conservative physical N16R8 normal/full-FX baseline of **24 Voices**. The first tested sustained unstable normal step is 28 Voices. The deliberately pathological all-features-on DSP class is stable through **8 Voices** and first unstable at 12 Voices.

The 64-frame period at 44.1 kHz is approximately 1451 us. In production-shaped continuous music stress this is a **soft transient timing target**, not a hard failure by itself. Hard realtime qualification is DMA/backlog-aware and fails on unrecovered backlog, DMA-window exhaustion, active playback queue overflow, write failure, clipping or runtime safety failure. Sustained polyphony sweeps remain strict boundary measurements.

## Fixed product baseline

- ESP32-S3 N16R8
- 16 MiB Flash
- 8 MiB Octal PSRAM
- ESP-IDF v6.1
- 240 MHz CPU
- 44.1 kHz / 16-bit audio
- stereo synth/mix path
- current Korvo V4 qualification output: mono downmix -> ES8311 -> PA -> speaker
- 16 MIDI channels / 16-part multitimbral GM runtime
- GM1 128 melodic programs + drums where present
- normal firmware PCM residency arena default remains 4 MiB unless separately changed
- physical qualification arena: 4.50 MiB
- MIDI velocity compatibility: 1..127
- software hard Voice cap: 32
- normal/full-FX physical nominal polyphony baseline: 24 Voices

R16 assumptions are prohibited. SoundFont file size itself is not the synth-engine limit.

## Product-format decision

Native `.sf2` is a first-class user asset. Users should be able to create/share a SoundFont with existing tools and import that same `.sf2` into the product. Firmware may generate disposable indexes/caches, but a private compiled bank is not required as the user-facing format.

SF3 is not a V1 priority.

## Validation layers

1. **Host tests** — deterministic SF2 semantics, MIDI/channel behavior, ownership/state machines, residency and DSP/runtime logic.
2. **ESP32-S3 QEMU** — Xtensa + ESP-IDF + FreeRTOS + VFS/FATFS + PSRAM integration and panic-path evidence.
3. **Physical N16R8** — actual memory reserve, storage timing, DSP cost, I2S/DMA behavior, codec/PA/speaker path and physical polyphony.
4. **Clean audible validation** — representative musical content used to find pitch/sample/loop/envelope/output corruption.
5. **Synthetic performance stress** — intentionally harsh workloads used to characterize limits, not to define all product behavior.

QEMU timing is never physical-performance evidence. Synthetic stress is never subjective audible-quality evidence.

## M0 — N16R8 hardware baseline

Status: **COMPLETE for the current qualification board.**

Established:

- ESP-IDF v6.1 / N16R8 build assertions;
- 16 MiB Flash / 8 MiB Octal PSRAM;
- physical boot with PSRAM at 80 MHz;
- internal-Flash -> PSRAM benchmark;
- 44.1 kHz I2S clocking and DMA startup;
- ES8311 control/readback;
- PA enable and speaker output.

Storage throughput is approximately 5.8–6.3 MiB/s across 4/8/16/32/64 KiB reads. 32 KiB remains the practical source-read chunk baseline.

## M1 — SF2 analyzer / compatibility inventory

Status: **COMPLETE / CI-backed core.**

Coverage includes RIFF/SF2 structure, source identity, sample metadata, bank/program distributions, generator/modulator usage, stereo links, dependencies, working-set estimates and compatibility reporting.

Parseability remains distinct from runtime/audio qualification.

## M2 — ResolvedZone / generator hierarchy

Status: **COMPLETE / CI-backed core.**

Flow:

`Preset global/local -> Instrument global/local -> Sample -> ResolvedZone`

Coverage includes range intersection, generator hierarchy, address/loop offsets, root key/tuning, envelopes, filter, pan, attenuation, FX sends and exclusive class.

## M2.5 — Modulator resolver / runtime modulation

Status: **CORE COMPLETE.**

Runtime coverage includes attenuation/pan, filter/resonance, volume envelope, modulation envelope, vibrato LFO, modulation LFO, pitch bend/range, modulation wheel, channel pressure and CC91/CC93 sends.

Representative user-SF2 banks with non-zero modulators remain an M7 release-compatibility target because the compact tracked FM bank itself contains no non-zero pmod/imod records.

## M3 — Sample residency / storage / Program Prepare

Status: **COMPLETE for core architecture and current physical qualification.**

Implemented path:

```text
native .sf2
  -> VFS RandomAccessReader
  -> Sf2SampleSource
  -> dependency set
  -> ProgramPrepareController
  -> direct-to-arena chunked loading
  -> Program Pins
  -> PublishedSampleDirectory
  -> immutable READY ProgramSnapshot
  -> block-boundary Commit
  -> move-only Voice Ref
```

Established behavior includes transactional rollback, safe LRU eviction, resident reuse, Active/READY exchange, release-tail survival, bounded capacity failure and a 16-channel latest-wins Program Change mailbox.

Realtime invariant: no filesystem/Flash/SD read in the audio hot path.

## M4 — Voice DSP + FX + physical audio

Status: **COMPLETE for current M6 baseline.**

Implemented paths include sample interpolation/phase stepping, pitch, looping, attenuation/pan, volume envelope, resonant/dynamic filtering, vibrato LFO, modulation LFO, modulation envelope, reverb/chorus sends, global FX, I2S DMA, ES8311 and PA output.

The clean real-SF2 automated physical path passes runtime/I2S invariants. Subjective listening remains a separate release-quality activity.

## M5 — Timestamped realtime event scheduler / GM channel runtime

Status: **COMPLETE / CI-backed core.**

Path:

`input/event producer -> timestamp -> bounded queue -> block scheduler -> frame-offset execution`

Established behavior includes Note On/Off, sustain, volume/pan/expression, pitch bend/range, channel pressure, Program Change handoff, GM Reset, 16 independent MIDI channel states, GM Channel 10 percussion policy and latest-wins Program Change handling.

## M6 — Adaptive polyphony / physical product qualification

Status: **COMPLETE.**

Checkpoint: `c16ff278`

### Normal/full-FX sustained physical boundary

- 16 Voices: avg 896 us / max 904 us — stable;
- 20 Voices: avg 1081 us / max 1091 us — stable;
- 24 Voices: avg 1268 us / max 1278 us — stable;
- 28 Voices: avg 1455 us / max 1465 us — first tested unstable step;
- 32 Voices: avg 1641 us / max 1650 us — unstable.

Product baseline: **24 normal/full-FX Voices**.

### Pathological heavy-DSP boundary

- 1 / 2 / 4 Voices: stable;
- 8 Voices: avg 1301 us / max 1312 us — stable;
- 12 Voices: avg 1878 us / max 1889 us — first unstable step.

Characterization baseline: **8 heavy-DSP Voices stable**.

### Final production-shaped music stress

`118` events / `16384` measured blocks:

- peak active Voices: 24;
- render avg/max: 754 / 1896 us;
- render soft overruns: 5;
- producer-work soft overruns: 6;
- max consecutive producer-work overruns: 2;
- max accumulated backlog: 448 us;
- final backlog: 0 us;
- DMA window: 11610 us;
- minimum DMA margin: 11162 us;
- active synth queue overflows: 0;
- producer stalls over DMA window: 0;
- write timeouts: 0;
- partial writes: 0;
- clipping: 0;
- hard-gate flags: 0;
- semantic stress: PASS.

Worst transient pure-render block remains 24 simultaneous NoteOn events at about 1896 us. This is an M7 headroom/regression metric, not an M6 blocker.

## M7 — GM128 / user-SF2 release qualification

Status: **ACTIVE.**

Detailed task: `tasks/M7_GM128_USER_SF2_RELEASE.md`.

### M7.0 — Tracked GM128 release matrix

Retain the existing five `gm128-full-bank` host gates and add a sixth per-Program release matrix using the production `Sf2PlaybackProgramPreparer`.

Required outputs:

- one machine-readable row per Bank 0 program 0–127;
- one row per Bank 128 program 0–127;
- zones / dependencies / aligned PCM working set;
- modulator and Preview state;
- representative melodic coverage;
- standard drum coverage;
- JSON release artifact;
- Markdown release artifact.

This is reporting/qualification consolidation, not a duplicate GM runtime implementation.

### M7.1 — Multi-bank native-SF2 release matrix

Generalize the same schema across representative native `.sf2` banks. Include a mainstream complete GM/GS bank and a structurally richer user-style bank containing features absent from the compact FM bank, especially stereo links and non-zero modulators.

### M7.2 — Real-bank 16-channel product workload

Qualify mixed 16-channel GM playback, dense chords, sustain/release, drum bursts, CC/pitch/FX changes, Program/Bank Change, GM Reset and rapid Program Change while other channels remain active.

### M7.3 — User-SF2 robustness

Bound and classify extreme, unsupported and malformed assets. Explicit rejection is acceptable; panic, invalid sample access, stale ownership or destructive eviction of live audio is not.

### M7.4 — Physical long-run / audible release sampling

Use physical N16R8 for representative-bank storage/Prepare timing, long I2S/DMA health, real p95/p99/max/backlog, memory high-water and selected audible release checks.

## Decision log

- Hardware target is ESP32-S3 N16R8; R16 assumptions are prohibited.
- ESP-IDF v6.1 is the embedded baseline.
- Native SF2 is the user-facing/shareable asset format.
- V1 GM runtime is 16 MIDI channels, not 32.
- MIDI channel count and simultaneous sample-Voice count are independent.
- Shared/per-program residency is mandatory; full residency is only an optimization.
- Asset I/O stays outside the audio hot path.
- Residency admission never destroys Active/READY/live-Voice ownership merely to force Program Change to fit.
- QEMU is an integration gate, not a physical timing oracle.
- Synthetic performance stress is not an audible-quality oracle.
- The software hard Voice cap is 32 and is not a guaranteed realtime specification.
- M6 normal/full-FX physical nominal polyphony is 24 Voices; 28 is the first tested sustained unstable step.
- M6 pathological heavy-DSP characterization is 8 Voices stable; 12 first unstable.
- 1451 us is a soft transient timing target for production-shaped buffered playback; sustained workload boundaries remain strict.
- Hard realtime qualification is based on recoverable backlog/DMA/write/queue/runtime health rather than a single isolated block overrun.
- The ~1.9 ms 24-NoteOn burst is retained as a regression/headroom metric and should only become an optimization priority if real M7 banks create materially worse backlog or product latency.
