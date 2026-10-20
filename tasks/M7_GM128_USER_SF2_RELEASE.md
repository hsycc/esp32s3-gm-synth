# M7 — GM128 / User-SF2 Release Qualification

Status: **ACTIVE**

M7 starts from the completed M6 physical baseline at `c16ff278b265018be9d2af5cc4fe3e54fe01e947`.

Current software/release-image checkpoint: `22b48b2f8ef58d01c3c0c39ddea47c7d40f330a9`.
The automated M7 N16R8 physical long-run passed on 2026-10-20; retained evidence is in `benchmarks/M7_PHYSICAL_LONGRUN_2026-10-20.md` and `docs/M7_RELEASE_QUALIFICATION.md`.

## Goal

Turn the existing SF2/GM engine from a physically qualified implementation into a repeatable release-qualified product path for the tracked GM128 bank and representative user-supplied native `.sf2` assets.

M7 does not redefine the M6 polyphony result. The N16R8 product envelope remains:

- normal/full-FX nominal physical polyphony: 24 Voices;
- first tested normal sustained unstable step: 28 Voices;
- pathological heavy-DSP stable maximum: 8 Voices;
- first pathological unstable step: 12 Voices;
- software hard cap: 32 Voices;
- transient same-block event overruns are soft diagnostics unless they create unrecovered DMA/backlog failure.

## M7.0 — Per-program release matrix

`tests/gm/test_gm128_release_matrix.cpp` runs the production `Sf2PlaybackProgramPreparer` over Bank 0 programs 0–127 and Bank 128 programs 0–127. `tools/gm128_release/report.py` emits machine-readable JSON and a human-readable Markdown release report.

The tracked matrix passes all 128 melodic programs and all 128 tracked percussion-bank programs without release-blocking coverage/qualification failures.

## M7.1 — Representative multi-bank native-SF2 matrix

The generalized release scanner classifies complete native `.sf2` banks program-by-program and keeps unsupported features as explicit release rejects.

Current full-bank checkpoint before the next compatibility expansion:

- FM/GM Open: 5760 / 5760 presets StrictQualified;
- GeneralUser GS: 243 / 287 StrictQualified, with 44 explicit rejects;
- FluidR3 GM: 173 / 189 StrictQualified, with 16 explicit rejects.

Current reject classes are explicit and measured rather than inferred:

- GeneralUser GS: 36 custom-modulator programs, 4 invalid-loop programs, 4 programs above the current 256-zone realtime bound;
- FluidR3 GM: 3 custom-modulator programs, 12 programs above the current 256-zone realtime bound, 1 scale-tuning program.

The next M7 software batch targets the observed custom-modulator shapes first and then a key-indexed snapshot for large programs; it must not simply raise the linear realtime scan bound.

## M7.2 — 16-channel product workload

The real-bank ownership gate exercises representative 16-part GM behavior including rapid Program Change while other channels remain active. It preserves Prepare -> READY -> block-boundary commit -> ACK semantics and checks published Voice-reference lifetime across the transaction.

Qualification preserves the M6 hard realtime model:

- 1451 us block overruns are soft timing diagnostics;
- unrecovered backlog, DMA-window exhaustion, active playback queue overflow, write failures, clipping, invalid sample access and ownership/lifetime faults are hard failures.

## M7.3 — User-SF2 robustness / edge cases

Coverage includes deterministic malformed/truncated RIFF rejection, large bank-local SampleIds independent of the 512 simultaneous-residency cap, real-bank issue-level telemetry, unlinked FluidR3 left/right-side normalization, and explicit large-program/modulator boundaries.

The required behavior remains explicit acceptance or explicit rejection, never uncontrolled memory access or realtime ownership corruption.

## M7.4 — Physical long-run / audible release sampling

A dedicated `sdkconfig.m7release.defaults` / `esp-idf-m7-release-build` path builds a separate physical release image without changing the established M6 16384-block hwbench baseline.

The 2026-10-20 N16R8 run completed both 131072-block windows. The clean real-SF2 window and the production-shaped 118-event music-stress window both passed the backlog-aware hard gate with `failure_flags=0`, zero producer stalls/write timeouts/partial writes/active synth queue overflows/clipping, zero final backlog, and no DMA-window exhaustion. The same run re-confirmed normal/full-FX 24 voices and pathological-heavy 8 voices as the stable physical envelope.

Subjective audible evidence remains separate because the current ES8311 qualification output is mono and cannot by itself establish spatial stereo/pan correctness.

## Exit criteria

- [x] tracked GM128 release matrix emits JSON + Markdown and is CI-backed;
- [x] all Bank 0 melodic programs pass the release matrix;
- [x] tracked Bank 128 percussion set passes the release matrix;
- [x] at least three representative native SF2 banks are classified with the same report model;
- [x] mixed 16-channel real-bank workload passes semantic and ownership gates;
- [x] qualified CI workloads show no invalid sample/fetch access, stale reference or leaked Voice refs;
- [x] M7 physical long-run confirms no supported workload causes unrecovered DMA/backlog/write failure;
- [x] unsupported/malformed user-SF2 conditions fail explicitly and safely;
- [ ] representative audible release checks are documented with actual hardware evidence;
- [ ] final public compatibility/polyphony claims are derived from completed representative physical and compatibility evidence rather than isolated synthetic maxima.
