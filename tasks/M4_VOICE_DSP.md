# M4 — Voice DSP Baseline

## Status

**In progress.** The physical N16R8 internal-Flash/PSRAM baseline has been captured, so M4 is no longer blocked on M3 storage qualification.

## Goal

Turn an already-resolved sample Voice into bounded 44.1 kHz stereo audio while preserving the M3 ownership contract:

```text
PublishedSampleDirectory
  -> move-only PublishedVoiceRef
  -> Voice start parameters
  -> sample interpolation / phase advance
  -> envelope + gain/pan + DSP
  -> int32 stereo mix accumulator
  -> clipped 16-bit stereo output
  -> I2S
```

The audio hot path must never perform SF2 parsing, filesystem access, Flash/SD reads, residency allocation, Program Prepare, or unbounded scans.

## Phase A — deterministic Voice core

- fixed-capacity Voice pool with a hard safety ceiling
- move-only `PublishedVoiceRef` lifetime held by each active Voice
- Q32.32 source phase and pre-resolved phase increment
- linear sample interpolation baseline
- one-shot, continuous-loop and loop-until-release behavior
- attack/decay/sustain/release baseline
- pre-resolved left/right gain into an int32 stereo accumulator
- exact rendered-voice-frame telemetry
- deterministic Host tests, including signed PCM and release-tail ownership

## Phase B — SF2 parameter bridge

- convert source sample rate + key/root-key/tuning to phase increment outside the per-frame hot path
- convert resolved attenuation/pan/velocity to Voice gains
- convert resolved envelope timecents/sustain values to runtime envelope coefficients/frames
- map SF2 sample modes to the runtime loop mode
- retain compatibility quirks as explicit policy rather than mutating parsed SF2 data

## Phase C — remaining Voice DSP

- low-pass filter where required
- vibrato/mod LFO
- modulation envelope
- pitch/filter/volume modulation destinations needed by the supported ModPlan
- dry/reverb/chorus sends
- stereo-linked sample handling

## Phase D — ESP32-S3 audio boundary

- fixed 64-frame render block baseline
- int32 mix -> saturated 16-bit stereo conversion
- ESP-IDF I2S at 44.1 kHz / 16-bit / stereo
- block render duration telemetry
- deadline miss / underrun counters
- physical N16R8 memory high-water and first stable-polyphony measurements

## Validation

Host:
- interpolation and loop boundaries are deterministic
- negative and positive PCM scaling is symmetric
- NoteOff release eventually releases the exact Voice Ref
- a release tail can survive Program retirement without stale PCM
- pool overflow is explicit and bounded

ESP32-S3 build/QEMU:
- Voice core compiles for Xtensa under pinned ESP-IDF v6.1
- existing real-SF2 storage/Prepare gates remain green
- no new panic/assert path is introduced

Physical N16R8:
- 44.1 kHz stereo output is stable
- render timing is measured against the 64-frame deadline
- no filesystem/storage call appears in the audio task
- first memory/DSP baseline is recorded before raising the production residency arena or claiming >16 qualified sample voices

## Exit criteria

- representative real-SF2 notes can traverse published PCM -> Voice DSP -> I2S
- deterministic Host tests cover the core interpolation/loop/envelope/lifetime semantics
- physical block timing and underrun telemetry exists
- 16 active sample voices have a measured qualification result, even if higher counts remain opportunistic
