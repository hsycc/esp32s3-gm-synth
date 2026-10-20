# M7 — GM128 / User-SF2 Release Qualification

Status: **ACTIVE**

M7 starts from the completed M6 physical baseline at `c16ff278b265018be9d2af5cc4fe3e54fe01e947`.

Current software/release-image checkpoint: `22b48b2f8ef58d01c3c0c39ddea47c7d40f330a9`.
See `docs/M7_RELEASE_QUALIFICATION.md` for the frozen software compatibility matrix and the remaining physical evidence contract.

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

The generalized release scanner now classifies complete native `.sf2` banks program-by-program and keeps unsupported features as explicit release rejects.

Current full-bank checkpoint:

- FM/GM Open: 5760 / 5760 presets StrictQualified;
- GeneralUser GS: 243 / 287 StrictQualified, with 44 explicit rejects;
- FluidR3 GM: 173 / 189 StrictQualified, with 16 explicit rejects.

The release blockers are documented in `docs/M7_RELEASE_QUALIFICATION.md`. In particular, M7 deliberately retains the 256-zone realtime scan bound rather than raising it to accommodate 320–720-zone presets without an indexed runtime design.

## M7.2 — 16-channel product workload

The real-bank ownership gate exercises representative 16-part GM behavior including rapid Program Change while other channels remain active. It preserves Prepare -> READY -> block-boundary commit -> ACK semantics and checks published Voice-reference lifetime across the transaction.

Qualification preserves the M6 hard realtime model:

- 1451 us block overruns are soft timing diagnostics;
- unrecovered backlog, DMA-window exhaustion, active playback queue overflow, write failures, clipping, invalid sample access and ownership/lifetime faults are hard failures.

## M7.3 — User-SF2 robustness / edge cases

Coverage now includes deterministic malformed/truncated RIFF rejection, large bank-local SampleIds independent of the 512 simultaneous-residency cap, real-bank issue-level telemetry, unlinked FluidR3 left/right side normalization and explicit large-program/modulator boundaries.

The required behavior remains explicit acceptance or explicit rejection, never uncontrolled memory access or realtime ownership corruption.

## M7.4 — Physical long-run / audible release sampling

A dedicated `sdkconfig.m7release.defaults` / `esp-idf-m7-release-build` path now builds a separate physical release image without changing the established M6 16384-block hwbench baseline.

The M7 image is configured for:

- 131072 measured 64-frame blocks (~190.22 s);
- N16R8 / 4.50 MiB PCM arena;
- 8 x 64-frame DMA descriptors;
- production-shaped music-stress phases distributed through the measured interval;
- the same backlog-aware hard gates as M6.

`tools/m7_release/physical_report.py` turns the resulting serial log into JSON/Markdown release evidence and fails on hard-gate, backlog, duration, clipping, stack, missing-telemetry or panic regressions.

The build image is CI-qualified, but M7 remains ACTIVE until this image is actually run on physical hardware and the separate audible-release observations are recorded.

## Exit criteria

- [x] tracked GM128 release matrix emits JSON + Markdown and is CI-backed;
- [x] all Bank 0 melodic programs pass the release matrix;
- [x] tracked Bank 128 percussion set passes the release matrix;
- [x] at least three representative native SF2 banks are classified with the same report model;
- [x] mixed 16-channel real-bank workload passes semantic and ownership gates;
- [x] qualified CI workloads show no invalid sample/fetch access, stale reference or leaked Voice refs;
- [ ] M7 physical long-run confirms no supported workload causes unrecovered DMA/backlog/write failure;
- [x] unsupported/malformed user-SF2 conditions fail explicitly and safely;
- [ ] physical long-run and representative audible release checks are documented with actual hardware evidence;
- [ ] final public compatibility/polyphony claims are derived from the completed representative physical evidence rather than isolated synthetic maxima.
