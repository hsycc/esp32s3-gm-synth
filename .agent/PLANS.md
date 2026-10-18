# Implementation Plan

This file is the durable execution plan for Codex/local development. Keep it aligned with measured evidence. Keep host/QEMU evidence, physical timing evidence, audible-quality evidence, and synthetic limit characterization distinct.

## Current project position

The project is in **M6 physical product qualification / polyphony-envelope characterization**.

The latest measured physical checkpoint remains `73a4765`:

- normal/full-FX synthetic workload: stable through 28 voices, first failed step at 32 voices;
- deliberately pathological all-features-on DSP workload: stable through 4 voices, first failed step at 8 voices.

A failed step in a voice-count sweep means **the tested workload crossed its realtime polyphony limit**. It does **not** mean the firmware or the whole M6 milestone failed. The purpose of the sweep is to find that boundary.

The current source checkpoint after the latest harness work is `d3bbd8b` (`test: move clean SF2 prepare workspace off main task stack`). It contains two important changes that still require the next physical run for measurement/audible confirmation:

1. the pathological heavy-DSP sweep stops at the first unstable step instead of continuing deeper into already-proven overload and provoking watchdog activity;
2. a separate real-SF2 clean audible gate runs before synthetic performance stress.

The clean gate uses the real SF2 Bank 0 / Program 0 path and a sparse dry musical sequence. It is implemented in source, but **physical audible sign-off is still pending**. Do not call it passed until the board run and listening/log evidence confirm it.

The existing I2S hardware-isolation 441 Hz reference generator is still a **triangle wave**, not a sine wave. It is useful for basic I2S -> ES8311 -> PA -> speaker continuity but is naturally harmonically rough and must not be treated as the final clean-tone oracle. Converting that reference to a true sine remains an audible-diagnostics cleanup item.

The next product decision is to define representative workload classes and choose a conservative nominal polyphony with engineering margin. A current working candidate for the normal/full-FX class is **24 voices**; 28 is a measured edge with too little timing margin to advertise directly.

## Fixed product baseline

- ESP32-S3 N16R8
- 16 MB flash
- 8 MB Octal PSRAM
- ESP-IDF v6.1
- 44.1 kHz / 16-bit audio
- stereo synth/mix path
- current Korvo V4 qualification speaker path: mono downmix -> ES8311 -> PA -> speaker
- **16 MIDI channels / 16-part multitimbral GM runtime**
- GM1 128 melodic programs + drums
- normal firmware PCM residency arena default: 4 MiB
- current dedicated physical qualification arena: 4.50 MiB
- MIDI velocity compatibility: 1..127

### Terminology

- **MIDI channel count**: 16, internal indices 0..15; GM Channel 10 is index 9.
- **Polyphony / active voices**: simultaneous sample voices; independent of MIDI channel count.
- one MIDI channel may consume several voices through chords, release tails, stereo/layered zones, etc.
- 16 MIDI channels therefore does not imply 16-voice or 32-voice polyphony.

R16 is excluded. SoundFont file size itself is not the synth-engine limit.

## Product-format decision

Native `.sf2` is a first-class user asset. Users should be able to create/share a SoundFont with existing tools and import the same `.sf2` into the product. Firmware may generate disposable indexes/caches, but a private compiled bank is not required as the user-facing format.

## Validation layers

Keep these evidence classes separate:

1. **Host tests** — deterministic SF2 semantics, MIDI/channel behavior, ownership/state machines and DSP/runtime logic.
2. **ESP32-S3 QEMU** — Xtensa + ESP-IDF + FreeRTOS + VFS/FATFS + PSRAM integration and panic paths.
3. **Physical N16R8** — actual memory reserve, storage timing, DSP cost, I2S deadlines, codec/PA/speaker behavior and stable polyphony.
4. **Clean audible validation** — musical content expected to sound clean, used to find pitch/sample/loop/envelope/output corruption.
5. **Synthetic performance stress** — intentionally harsh workloads used to characterize CPU/deadline limits.

QEMU timing is not physical-performance evidence. A harsh synthetic fixture is not audible-quality evidence.

## M0 — N16R8 hardware baseline

Status: **established for the current qualification board.**

Established:
- ESP-IDF v6.1 / N16R8 build assertions;
- 16 MiB Flash / 8 MiB Octal-PSRAM configuration;
- physical boot with 8 MiB PSRAM at 80 MHz;
- physical internal-Flash -> PSRAM benchmark;
- 44.1 kHz I2S clock generation and DMA startup;
- ES8311 control/readback;
- PA enable and audible speaker output.

Measured internal-Flash -> PSRAM throughput is approximately 5.8–6.3 MiB/s across the 4/8/16/32/64 KiB sweep. 32 KiB remains the practical source chunk baseline.

## M1 — SF2 analyzer

Status: **established and CI-backed.**

Coverage includes RIFF/SF2 structure, source identity, `smpl`, preset/instrument/sample counts, bank/sample-rate distributions, generator/modulator usage, stereo links, per-program dependencies, realistic 16-channel working sets and N16R8 residency estimates.

## M1.5 — SF2 compatibility baseline

Status: **core compatibility reporting established.**

Parseability remains separate from runtime/audio qualification. FM/GM Open, GeneralUser GS, FluidR3_GM, TimGM6mb and GMBK5X128 serve different compatibility/stress roles.

## M2 — Generator hierarchy and ResolvedZone

Status: **implemented and CI-backed.**

Flow:

`Preset global/local -> Instrument global/local -> Sample -> ResolvedZone`

Coverage includes range intersection, generator contribution/override rules, sample address/loop offsets, root key/tuning, envelopes, filter, pan, attenuation and FX sends.

## M2.5 — Modulator resolver / runtime modulation

Status: **core resolver established; major dynamic modulation paths integrated.**

Runtime coverage includes attenuation/pan, filter/resonance, volume envelope, modulation envelope, vibrato LFO, modulation LFO, pitch bend/range, modulation wheel, channel pressure and CC91/CC93 sends.

## M3 — Sample residency / storage / Program Prepare

Status: **core architecture, QEMU integration and current physical internal-Flash/Program-Prepare qualification are complete.**

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

Established behavior includes transactional loading/rollback, safe LRU eviction, resident reuse, READY/Active exchange, release-tail survival, explicit non-destructive capacity failure and a **16-channel** latest-wins Program Change mailbox.

Realtime invariant: no filesystem/Flash/SD read in the audio hot path.

## M4 — Voice DSP + FX + physical audio

Status: **DSP paths are implemented and physically exercised; dedicated clean-audible source path is now implemented, with physical sign-off pending.**

Implemented DSP/audio paths include sample interpolation/phase stepping, pitch conversion, looping, attenuation/pan, volume envelope, resonant/dynamic filtering, vibrato LFO, modulation LFO, modulation envelope, reverb/chorus sends, global FX, I2S DMA, ES8311 and PA output.

### Clean audible gate — source implementation at `d3bbd8b`

The physical qualification image now runs a clean path before synthetic stress:

```text
real /sf2/test.sf2
  -> Sf2PdtaReader + Sf2SampleSource
  -> Sf2PlaybackProgramPreparer
  -> Bank 0 / Program 0 PlaybackProgramSnapshot
  -> resident sample reuse/pin/publish
  -> RealtimeProgramEventApplier / ZoneResolver
  -> VoicePool
  -> ScheduledAudioBlockRenderer
  -> mono downmix
  -> I2S -> ES8311 -> PA -> speaker
```

The first audible SF2 sequence is deliberately sparse and dry:

- C4 single note for about 2 s;
- silence;
- C4/E4/G4 major chord for about 3 s;
- silence;
- C4 -> E4 -> G4 -> C5 sequence.

FX sends are cleared only for this first clean oracle. Sample region, tuning, loop, volume envelope, filter and native modulation state still come from the real prepared SF2 Program 0.

The large `Sf2SampleSource` + prepare workspaces are allocated from PSRAM instead of the main task stack. The realtime queue, publication atomics and realtime runtime remain in internal RAM. A compile-time size guard prevents the clean realtime aggregate from silently growing beyond the current internal-RAM budget.

Physical pass evidence still required:
- clean SF2 `i2s_ok=1`;
- clean runtime invariants pass;
- no clipping;
- no event rejects / missing program / unmatched zones / resolve overflow / voice-start failure;
- subjective listening confirms correct pitch, clean single note/chord, sensible attack/loop/release;
- no watchdog event.

After the initial dry oracle passes, add filter/modulation/FX one at a time rather than enabling the entire pathological feature set at once.

## M5 — Timestamped realtime event scheduler / GM channel runtime

Status: **core runtime implemented and CI-backed.**

Path:

`input/event producer -> timestamp -> bounded queue -> block scheduler -> frame-offset execution`

Established behavior includes Note On/Off, CC, sustain, pitch bend/range, channel pressure, Program Change handoff, GM Reset, 16 independent MIDI channel states, GM Channel 10 percussion policy and latest-wins Program Change handoff.

## M6 — Adaptive polyphony / physical product qualification

Status: **ACTIVE — current focus is measuring and classifying physical polyphony limits.**

Block budget at 44.1 kHz / 64 frames is approximately 1451 us.

### Latest measured checkpoint: `73a4765`

Normal/full-FX synthetic workload:

- 16 voices: max 887 us, zero deadline misses;
- 20 voices: max 1074 us, zero deadline misses;
- 24 voices: max 1252 us, zero deadline misses;
- 28 voices: max 1437 us, zero deadline misses;
- 32 voices: max 1616 us, 128/128 deadline misses;
- **measured stable maximum = 28 voices**;
- **first failed step = 32 voices**.

28 voices is a measured edge, not a safe public specification: it leaves only ~14 us against the block budget.

Deliberately pathological all-features-on DSP workload:

- 1 voice: stable;
- 2 voices: stable;
- 4 voices: stable;
- 8 voices: ~1513 us average, 128/128 deadline misses;
- **measured stable maximum = 4 voices**;
- **first failed step = 8 voices**.

This heavy fixture intentionally combines resonant filter, dynamic filter, vibrato LFO, Mod LFO -> pitch/filter/volume, Mod Envelope -> pitch/filter, chorus and reverb. Treat `heavy_dsp_max=4` as a worst-case complexity-class result, not automatically as the product's advertised nominal polyphony.

### Interpretation rule

A polyphony sweep should be read as:

```text
PASS ... PASS -> first FAIL
                 ^
                 boundary found
```

The first failed step is expected and useful.

### Harness changes now implemented

- heavy-DSP sweep stops after the first unstable step;
- results explicitly report `max_stable_voices`, `first_failed_voices` and `boundary_reached`;
- pathological heavy result no longer computes the product candidate as `min(normal_max, heavy_max)`;
- current log reports separate normal/full-FX maximum, nominal candidate, pathological heavy maximum and first unstable step;
- an idle scheduling point separates the heavy sweep from the later continuous-I2S phase;
- clean real-SF2 audible gate runs before synthetic performance stress.

Expected next-board heavy sequence from the current measured envelope is therefore:

```text
1 PASS -> 2 PASS -> 4 PASS -> 8 first unstable -> STOP
```

It should no longer continue 12/16/20/24/28/32 merely to prove increasingly severe overload.

### Long aggressive I2S/music timeline

The existing long timeline intentionally mixes sustain/release/percussion/pitch/modulation/Program Change/GM reset and dense bursts. Its old `11338/16384` deadline misses and thousands of send-queue overflows are evidence that **that timeline crossed the realtime envelope**; they are not proof that a light/normal audio path is inherently broken.

However, the current code still treats this aggressive long-I2S return value as a hard benchmark result. The next harness cleanup should separate:

1. **clean/light I2S correctness gate** — must pass;
2. **selected nominal workload continuous-I2S gate** — must pass for the product polyphony claim;
3. **aggressive timeline stress** — characterize and report its boundary without confusing it with the nominal product gate.

Do not simply ignore genuine I2S/write/clock failures: the distinction is based on workload class and the already-passing clean gate, not on suppressing error telemetry.

### Product polyphony classes

Track at least:

- **simple/dry SF2**;
- **normal GM + FX**;
- **DSP-heavy SF2**;
- **pathological all-features-on stress**.

The eventual public specification should be a conservative **nominal polyphony** supported across representative real-bank workloads, with dynamic/adaptive reduction for unusually expensive patches if needed.

Current working direction:
- normal/full-FX measured edge: 28;
- normal nominal candidate for further continuous-I2S/real-bank validation: **24**;
- pathological heavy ceiling: 4;
- hard software voice cap: 32.

This is not a final product specification until representative real-SF2 workloads and continuous I2S output confirm margin.

### M6 sign-off criteria

- clean audible gate passes physically;
- representative workload classes have measured stable maxima and first-failed points;
- selected nominal product workload runs continuously through physical I2S with no unacceptable underrun/queue/deadline failure;
- no unexpected clipping;
- no watchdog caused by the supported nominal workload;
- product polyphony is chosen with engineering margin from representative physical evidence;
- pathological stress limits are documented separately rather than forcing the public nominal specification down to their worst-case number.

## M7 — GM128 / user-SF2 release qualification

Status: **partial CI coverage exists; full product qualification follows M6 workload-class and nominal-polyphony decisions.**

Targets:
- 128 GM melodic programs + drums where provided;
- **16 MIDI channels**;
- Program/Bank change;
- Note On/Off, sustain, volume, pan, expression;
- pitch bend/range;
- reverb/chorus sends;
- default/custom modulators;
- stereo-linked samples;
- dense chords/sustain, drum bursts, rapid Program Change and mixed 16-channel working sets;
- long-running playback over multiple representative public SF2 banks.

Release metrics include deadline misses/underruns/queue drops, invalid sample accesses, max/P95/P99 render time, internal SRAM/PSRAM high-water, residency pressure, load/reuse/eviction latency, stable polyphony by DSP complexity class and clean audible correctness.

## Current decision log

- Hardware target is ESP32-S3 N16R8; R16 assumptions are prohibited.
- ESP-IDF v6.1 is the embedded baseline.
- Native SF2 is the user-facing/shareable asset format.
- V1 GM runtime is **16 MIDI channels**, not 32.
- MIDI channel count and simultaneous voice/polyphony count are independent.
- Shared/per-program residency is mandatory; full residency is only an optimization.
- SF3 is not a V1 priority.
- Native SF2 sample loading is random-access/chunked and control-domain only.
- Residency admission never destroys Active/READY/live-Voice ownership merely to force a Program Change to fit.
- QEMU is an integration gate, not a physical performance oracle.
- Synthetic performance stress is not an audible-quality oracle.
- A failed voice-count step after lower passing steps is a measured performance boundary, not automatically a product or milestone failure.
- Current measured boundaries: **normal/full-FX stable to 28, first fail 32; pathological heavy-DSP stable to 4, first fail 8**.
- Current normal nominal candidate for further validation is **24 voices**; 28 is an edge measurement, not a product promise.
- Public nominal polyphony will be selected from representative real workload classes with engineering margin, not from the highest isolated voice count and not from the pathological minimum alone.
