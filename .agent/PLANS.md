# Implementation Plan

This file is the durable execution plan for Codex/local development. Keep it aligned with measured evidence. Do not describe failed qualification as complete, and do not collapse host/QEMU evidence, physical timing evidence and audible-quality evidence into one claim.

## Current project position

The project is in **M6 physical product qualification**, but the latest physical checkpoint is **not a pass**.

Checkpoint `73a4765` established that the basic physical audio path still initializes correctly, but the extended heavy-DSP and long I2S/music stress expose serious realtime failures:

- normal/full-FX synthetic polyphony: 28 voices completes the 1451 us block gate, but with very little timing margin; 32 voices misses every measured deadline;
- deliberately heavy-DSP profile: 1/2/4 voices pass, 8 voices and above miss every measured deadline; current `heavy_dsp_max=4` versus the qualification requirement of 16;
- the heavy 32-voice step triggers the task watchdog on CPU0 while rendering;
- extended physical I2S/music stress triggers the task watchdog repeatedly on CPU1;
- extended I2S/music stress reports 11338 render deadline misses, thousands of send-queue overflows, producer stalls beyond the DMA window, and ends in `i2s_qual: FAIL`;
- digital clipping remains zero, so the latest audible breakup/noise must not be explained away as simple int16 clipping.

Therefore the current priority is **realtime failure isolation + clean audible validation**, not choosing or advertising a final product polyphony number.

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
- current dedicated physical qualification arena: 4.50 MiB
- engine MIDI velocity compatibility: 1..127

### Terminology

- **MIDI channel count**: 16. Internal indices are 0..15. GM Channel 10 is index 9.
- **Polyphony / active voices**: simultaneous sample voices; independent of MIDI channel count.
- One MIDI channel can consume multiple voices because of chords, release tails and layered SF2 zones.
- 16 MIDI channels does not imply 16-voice or 32-voice polyphony.

R16 is excluded. SoundFont file size is not itself the synth-engine limit.

## Product-format decision

Native `.sf2` is a first-class user asset format. Users should be able to create/share a SoundFont with existing tools and import the same `.sf2` into the product. Firmware may generate disposable indexes/caches, but a private compiled bank is not the required user-facing path.

## Validation layers

Keep these evidence classes separate:

1. **Host tests** — deterministic SF2 semantics, MIDI/channel behavior, ownership/state machines and DSP/runtime logic.
2. **ESP32-S3 QEMU** — Xtensa + ESP-IDF + FreeRTOS + VFS/FATFS + PSRAM integration and panic paths.
3. **Physical N16R8** — actual memory reserve, Flash/optional-SD timing, DSP cost, I2S deadlines, codec/PA/speaker behavior and stable polyphony.
4. **Clean audible validation** — normal musical content that is expected to sound clean and is suitable for detecting pitch/sample/loop/envelope/output corruption.
5. **Synthetic performance stress** — intentionally harsh workloads used only to characterize CPU/deadline headroom.

QEMU throughput/latency is never physical performance evidence. A harsh synthetic fixture is never audible-quality evidence.

## M0 — N16R8 hardware baseline

Status: **basic board/storage/audio bring-up established. Long realtime qualification belongs to M6 and currently fails.**

Established:
- ESP-IDF v6.1 and N16R8 build assertions;
- 16 MiB Flash / 8 MiB Octal-PSRAM configuration;
- physical boot detects 8 MiB Octal PSRAM at 80 MHz;
- physical internal-Flash -> PSRAM benchmark;
- physical 44.1 kHz I2S clock generation and DMA startup;
- ES8311 control/readback;
- PA enable and audible speaker output.

Measured internal-Flash -> PSRAM throughput is approximately 5.8–6.3 MiB/s across the 4/8/16/32/64 KiB sweep. 32 KiB remains the practical source chunk baseline.

## M1 — SF2 analyzer

Status: **established and CI-backed.**

Coverage includes RIFF/SF2 structure, source identity, `smpl`, preset/instrument/sample counts, bank/sample-rate distributions, generator/modulator usage, stereo links, per-program dependencies, 16-channel working sets and N16R8 residency estimates.

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

Status: **DSP paths are implemented and physically exercised, but clean audible correctness is not yet signed off.**

Implemented paths include sample interpolation/phase stepping, pitch conversion, looping, attenuation/pan, volume envelope, resonant/dynamic filtering, vibrato LFO, modulation LFO, modulation envelope, reverb/chorus sends, global FX, I2S DMA, ES8311 and PA output.

### Required clean audible gate

The current performance fixtures are intentionally pathological and cannot be used to judge normal sound quality. Add a separate physical audible path using real SF2 content:

1. deterministic clean reference tone;
2. one real SF2 melodic note, dry/no FX;
3. small musical chord, dry/no FX;
4. attack/loop/release sanity;
5. filter enabled by itself;
6. modulation paths enabled one at a time;
7. chorus/reverb enabled last.

For every clean stage record PCM peak, clipping and checksum. If the reference tone itself is distorted/noisy, investigate I2S slot/frame formatting, codec level, PA level and physical output before blaming SF2/DSP.

## M5 — Timestamped realtime event scheduler / GM channel runtime

Status: **core runtime implemented and CI-backed.**

Path:

`input/event producer -> timestamp -> bounded queue -> block scheduler -> frame-offset execution`

Established behavior includes Note On/Off, CC, sustain, pitch bend/range, channel pressure, Program Change handoff, GM Reset, 16 independent MIDI channel states, GM Channel 10 percussion policy and latest-wins Program Change handoff.

## M6 — Adaptive polyphony / physical product qualification

Status: **ACTIVE AND CURRENTLY FAILING PHYSICAL QUALIFICATION.**

### Latest measured checkpoint: `73a4765`

Block budget at 44.1 kHz / 64 frames is 1451 us.

Normal/full-FX synthetic polyphony:

- 16 voices: max 887 us, zero deadline misses;
- 20 voices: max 1074 us, zero deadline misses;
- 24 voices: max 1252 us, zero deadline misses;
- 28 voices: max 1437 us, zero deadline misses, but only 14 us below the block budget and therefore not acceptable as a conservative public specification;
- 32 voices: max 1616 us and 128/128 deadline misses;
- measured fast/full-FX maximum: 28, with inadequate product margin at that top step.

Deliberately heavy-DSP profile (resonant filter + dynamic filter + vibrato LFO + Mod LFO pitch/filter/volume + Mod Envelope pitch/filter + chorus + reverb):

- 1 voice: stable;
- 2 voices: stable;
- 4 voices: stable;
- 8 voices: ~1513 us average and 128/128 deadline misses;
- 12/16/20/24/28/32 voices: all measured blocks miss deadline;
- current `heavy_dsp_max=4`;
- current heavy minimum requirement is 16;
- current `physical_product` result is FAIL.

At the 32-voice heavy step the task watchdog fires on CPU0 while execution is inside Voice rendering. The watchdog backtrace/logging can contaminate the extreme max-latency number, but it does not explain away the underlying failure because the 8-voice step already exceeds the 1451 us budget without needing that outlier.

Extended physical I2S/music stress:

- codec readback and physical clocks pass;
- DMA preload passes;
- task watchdog fires repeatedly on CPU1 while `gm_i2s_qual` owns the core;
- `synth_send_queue_overflows=4226`;
- `active_send_queue_overflows=4231`;
- `render_deadline_misses=11338` out of 16384 measured blocks;
- `cycle_over_budget=11338`;
- `producer_stalls_over_dma_window=4`;
- max render time reaches 19332 us and max cycle time 19342 us; watchdog/backtrace activity may inflate the worst outliers, but the sustained miss count remains a real failure;
- write timeouts = 0;
- partial writes = 0;
- digital clipped samples = 0;
- final result: `i2s_qual: FAIL`.

The latest audible breakup/noise is therefore consistent with a realtime scheduling/deadline failure. It must not be attributed only to the intentionally dissonant stress fixture.

### Immediate M6 work order

1. **Remove watchdog interference from the qualification harness without hiding real deadline misses.** The realtime/benchmark tasks must allow the configured idle watchdog contract to be satisfied, or the qualification-specific watchdog policy must be made explicit. Do not simply feed/disable the watchdog and call the workload healthy.
2. **Add the clean audible gate** described in M4 so audible correctness is independently measurable.
3. **Re-run the long I2S test at a known light workload first** and require zero active send-queue overflow, zero producer stalls and zero deadline misses.
4. **Re-run a controlled voice-count sweep through the physical I2S path**, rather than only offline render timing, to establish the continuous-output limit.
5. **Profile and optimize the heavy per-voice path**, especially dynamic filter/LFO/mod-envelope/effects processing, before trying to recover the 16-heavy-voice target.
6. Separate product complexity classes if the deliberately pathological all-features-on fixture is not representative of supported real-bank workloads, but do not relax the requirement without real-SF2 evidence.
7. Only after clean audio and realtime stability pass, choose a conservative public polyphony number with engineering margin.

### M6 sign-off criteria

- clean audible gate passes;
- no watchdog event during qualification;
- long physical I2S/music stress has zero unacceptable DMA/event-queue overflow, zero producer stall and zero render deadline miss for the selected product workload;
- no unexpected clipping;
- normal and heavy/realistic-complexity profiles have measured stable maxima;
- product polyphony is chosen from measured physical evidence with engineering margin, not from the highest isolated voice count.

The original qualification goal remains at least 16 active sample voices for supported product workloads. The current checkpoint does **not** meet the heavy-profile version of that goal.

## M7 — GM128 / user-SF2 release qualification

Status: **partial CI coverage exists; full product qualification waits for M6 realtime stability and clean-audio sign-off.**

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

- Hardware target is fixed: ESP32-S3 N16R8; R16 assumptions are prohibited.
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
- The current `73a4765` heavy-DSP and extended-I2S physical checkpoint is a **FAIL**, not a product-sign-off result.
- Do not set the public polyphony specification until the realtime failures above are resolved and clean audible validation passes.
