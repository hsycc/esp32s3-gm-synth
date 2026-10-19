# M7 — GM128 / User-SF2 Release Qualification

Status: **ACTIVE**

M7 starts from the completed M6 physical baseline at `c16ff278b265018be9d2af5cc4fe3e54fe01e947`.

## Goal

Turn the existing SF2/GM engine from a physically qualified implementation into a repeatable release-qualified product path for the tracked GM128 bank and representative user-supplied native `.sf2` assets.

M7 does not redefine the M6 polyphony result. The starting N16R8 product envelope is:

- normal/full-FX nominal physical polyphony: 24 Voices;
- first tested normal sustained unstable step: 28 Voices;
- pathological heavy-DSP stable maximum: 8 Voices;
- first pathological unstable step: 12 Voices;
- software hard cap: 32 Voices;
- transient same-block event overruns are soft diagnostics unless they create unrecovered DMA/backlog failure.

## M7.0 — Per-program release matrix

The existing `gm128-full-bank` lane already provides strong aggregate host evidence:

- all tracked presets StrictQualified;
- Bank 0 melodic programs 0–127;
- Bank 128 percussion programs 0–127;
- real Program Change transactions;
- 4 MiB cache-pressure sweeps;
- Bank Select and latest-wins Program Change;
- mixed 16-channel runtime behavior;
- Active+READY capacity bracketing;
- release-tail discovery;
- deterministic 32-second host soak.

M7.0 adds the missing release-reporting layer rather than duplicating those tests.

### New program matrix

`tests/gm/test_gm128_release_matrix.cpp` runs the production `Sf2PlaybackProgramPreparer` over:

- Bank 0 programs 0–127;
- Bank 128 programs 0–127.

Each `M7_PROGRAM` row records:

- bank / program / class;
- StrictQualified prepare status;
- channel policy;
- native and playback zone counts;
- sample dependency count;
- stereo-pair count;
- PCM and aligned PCM working-set bytes;
- supported / unsupported modulator counts;
- Preview-zone count;
- melodic representative key coverage;
- standard GM drum coverage for Bank 128 / Program 0.

`tools/gm128_release/report.py` converts the matrix into machine-readable JSON and a human-readable Markdown release report. The CI artifact is intended to become the stable format reused by later multi-bank qualification.

M7.0 passes only when all 128 melodic and all 128 tracked percussion-bank programs are present, prepared successfully, and contain no release-blocking coverage/qualification failures.

## M7.1 — Representative multi-bank native-SF2 matrix

Next, generalize the M7.0 runner so the same report schema can qualify multiple native `.sf2` assets without requiring a private compiled-bank format.

Representative classes should include:

- the tracked compact FM/GM bank;
- GeneralUser GS or another complete mainstream GM/GS bank;
- a larger or structurally more complex user-style SF2 with stereo/modulation features.

For each bank record:

- discovered Bank/Program coverage;
- prepare pass/failure reasons;
- dependency and working-set distribution;
- maximum zones / overlap / PCM working sets;
- supported and unsupported SF2 feature usage;
- cache/reuse/eviction behavior;
- programs requiring special release notes.

A user bank may be rejected for an explicit unsupported or malformed condition. It must not cause panic, invalid sample access, ownership corruption or destructive eviction of live audio state.

## M7.2 — 16-channel product workload

Exercise representative real-bank 16-part GM playback rather than only isolated programs:

- 15 melodic channels + GM Channel 10 percussion;
- Note On/Off and sustain;
- CC7 volume / CC10 pan / CC11 expression;
- pitch bend and bend range;
- CC91 reverb / CC93 chorus;
- Program and Bank Change;
- GM Reset;
- dense chords, drum bursts and release accumulation;
- rapid Program Change while other channels remain active.

Qualification should preserve the M6 hard realtime model:

- report 1451 us block overruns as soft timing diagnostics;
- hard-fail unrecovered backlog, DMA-window exhaustion, active playback queue overflow, write failures, clipping, invalid sample access and ownership/lifetime faults.

## M7.3 — User-SF2 robustness / edge cases

Add bounded negative and edge-case coverage for:

- large samples and high sample rates;
- very short / long loops;
- stereo linked samples;
- large zone and modulator tables;
- extreme envelopes and tuning;
- empty or missing presets / drum kits;
- capacity admission failure;
- Program Change under eviction pressure;
- release-tail survival;
- truncated or malformed SF2 input.

The required behavior is explicit acceptance or explicit rejection, never uncontrolled memory access or realtime ownership corruption.

## M7.4 — Physical long-run / audible release sampling

Use physical N16R8 for the things host/QEMU cannot prove:

- physical Program Prepare/storage latency on representative banks;
- long-running I2S/DMA health;
- real p95/p99/max render timing and backlog;
- real memory high-water;
- representative audible checks for pitch, loops, envelope behavior, drums, stereo/pan and FX;
- final release polyphony wording by workload class.

Subjective listening evidence must remain separate from automated timing evidence.

## Exit criteria

- [ ] tracked GM128 release matrix emits JSON + Markdown and is CI-backed;
- [ ] all Bank 0 melodic programs pass the release matrix;
- [ ] tracked Bank 128 percussion set passes the release matrix;
- [ ] at least three representative native SF2 banks are classified with the same report model;
- [ ] mixed 16-channel real-bank workload passes semantic and ownership gates;
- [ ] no invalid sample/fetch access, stale reference or leaked Voice refs;
- [ ] no supported workload causes unrecovered DMA/backlog/write failure;
- [ ] unsupported/malformed user-SF2 conditions fail explicitly and safely;
- [ ] physical long-run and representative audible release checks are documented;
- [ ] final public compatibility/polyphony claims are derived from representative physical evidence rather than isolated synthetic maxima.
