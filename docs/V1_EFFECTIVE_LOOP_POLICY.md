# Effective Zone loop review — Host batch v2

Status: experimental review, no firmware integration and no audio-quality approval.
The existing raw-header scanner and three-reference-sample planner remain unchanged
as legacy diagnostics. Use the new command for risk-filtered prepared-zone analysis.

## Why this batch

The submitted 128-header survey reported 109 Direct and 19 seam-only candidates.
Ten selected candidates carried provisional review flags; the report retained no
alternative candidates. Its shdr loops were not necessarily actual playback loops.
These counts describe the OLD submitted survey, not the results of this new code.

## Actual geometry, not another SF2 parser

`tools/export_prepared_loops.cpp` links the existing `Sf2PlaybackProgramPreparer`
and emits its StrictQualified PlaybackZones. The production resolver owns generator
hierarchy, offsets, modes, valid ranges and loop-start normalization. The Python
consumer reads exactly the resulting absolute PCM loop range. Rejected programs
remain rejected and are retained in the report; no raw-shdr fallback is invented.

Non-looping Zones, non-mono source sample types, NoteOn address-modulated Zones,
and regions outside the bounded analysis budget are explicitly excluded from
crossfade proposals. Until-release loops are analyzed only in their looping phase;
this batch does not implement any note-off/release or stereo-pair fade behavior.

## Separate three meanings

- `seam_best_fade_frames`: best meaningful seam reduction, before risk filtering.
- `selected_fade_frames`: best remaining Host candidate after all provisional gates.
- `runtime_fade_frames=0`, `runtime_approved=false`: no result is a DSP enable command.

A risky best candidate does not disqualify all lengths: alternatives are evaluated.
Every valid candidate keeps benefit/context/risk rejection lists. Illegal fade
lengths are recorded separately. No benefit, unknown context and risk rejection
have different Direct dispositions; NOT_SAMPLED is never reported as Direct/PASS.

Provisional review gates: keep the old meaningful-seam gates (2500 counts, 1800
absolute / 65% relative improvement), but require period change <1%, local DFT
distance <0.3, defined/unchanged local lag, no larger neighborhood peak, and at most
5% higher neighborhood first-difference RMS. These are conservative engineering
settings, NOT calibrated perception thresholds. Undefined data cannot be a pass.
Lag=0 does not prove pitch preservation; the overlap still shortens each steady
loop by N source frames. Intentional waveform edges must not be treated as defects.

The legacy metrics used len(loop)+1 rendered samples: their neighborhood had only
one sample after the wrap. New decision metrics use a complete symmetric window;
the one-sided metrics remain explicitly labelled for comparison. Do not silently
compare the old RMS values as if they used the same window. Local DFT/lag features
remain short-window diagnostics, not full pitch/timbre certification.

Geometry keys contain the asset SHA-256, sample identity/type, effective source and
loop ranges, mode, dynamic-address flag and policy version. All Zone references
are preserved when geometries are identical. This is report deduplication, not
an implemented embedded cache or immutable plan publication protocol.

## Run once or use CI artifacts

```sh
bash scripts/build_v1_loop_exporter.sh
V1_LOOP_EXPORTER="$PWD/build/v1-loop-host/export_prepared_loops" \
  python3 -m unittest discover -s tests -p 'test_v1*loop*.py' -v
python3 -m tools.v1_effective_loop_survey assets/sf2/full/GeneralUser-GS.sf2 \
  --exporter build/v1-loop-host/export_prepared_loops --limit 128 \
  --output build/v1-loop-host/generaluser-effective.json
```

The dedicated `v1-effective-loop-host` workflow builds the exporter, runs negative
and production offset/mode/clamp fixtures, and scans the checked-in GeneralUser
bank. Its artifact includes JSON with ALL candidate diagnostics/rejection reasons
and Markdown. No ESP-IDF build/flash, raw PCM or SF2 modification is performed.
The old claim that the full GeneralUser bank is absent from CI is superseded for
this dedicated job, which explicitly checks out and reads the existing asset.

Source bounds, successful subprocess termination, manifest completeness and
before/after asset hashes are checked. Reports contain the bank and exporter
hashes. A missing/malformed export fails explicitly rather than degrading to a
raw-header recommendation. Sample names come from the production decoded header;
no sample-ID/name whitelist is used.

## Remaining gates

Examine actual CI reports before deciding whether this policy is too conservative.
This is not whole-bank/GM-note, stereo, dynamic-address, playback-pitch or subjective
validation. Audio-domain fixed-point execution and lifetime/publication tests are
still separate work; accepted M8 DSP, defaults, DMA and performance gates are unchanged.
