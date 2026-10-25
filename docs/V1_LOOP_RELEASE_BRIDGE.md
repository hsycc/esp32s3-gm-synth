# Experimental bounded loop-release bridge

## Scope and frozen baseline

`BoundedLoopReleaseCursor` is a separate, explicitly opt-in wrapper around the
unchanged `LoopCrossfadeCursor` from `48a67182`. It is not referenced by production
Voice, a build configuration, or Program Prepare. The `de684b82` Host policy,
thresholds, soundbanks, gains, DMA and polyphony are unchanged. Both PRs remain
Draft; this work does not authorize a merge or any runtime plan.

The original cursor and its immediate-release oracle remain unchanged and are
retested in CI. Default release bridging is disabled. Direct (fade=0), None,
Continuous, and disabled configurations retain their old PCM and phase behavior.

## Contract

The caller must serialize control/render calls and pin immutable mono PCM through
the complete primary lifetime. Both cursors borrow that same buffer; this is NOT
SampleRef, generation validation, immutable plan publication or live Voice integration.

On an UntilRelease NoteOff the primary stops wrapping immediately without moving
its current source phase. If loop crossfade is enabled, the release bridge is
explicitly enabled, and held/raw values differ at that phase, a shadow of the
held-loop cursor supplies the other input of a short convex blend. The shadow may
wrap; the PRIMARY never wraps after NoteOff. Sustain and amplitude-envelope
routing remain upstream. There is no extra full loop or delayed NoteOff.

For requested maximum R (16, 32 or 64 OUTPUT frames), compute available raw
output frames at NoteOff with overflow-safe ceil((end-phase)/step). Choose
D=min(R, available). For D>=2 and j=0..D-1:

    output[j] = nearest((held[j] * (D-1-j) + raw[j] * j) / (D-1))

The first sample equals the held continuation at the SAME phase, not the previous
output sample; the last equals raw. This removes a branch-switch mismatch, not the
natural slope of the waveform. Integer nearest rounding is symmetric; exact
half-way ties (possible after tail-length adjustment) round away from zero.
The convex output stays within its two PCM16 inputs, without saturation or a
corrective DC offset. The raw tail keeps its phase and original end time.

These output-frame durations are independent of the 16/32/64 SOURCE-frame loop
crossfade. No global duration is approved from this experiment.

### Tail-end and pitch changes

A predictable short tail uses a COMPLETE shorter ramp, marked by
`transitionTailLimited()`, not a fixed ramp cut off at the last instant. The planned
output count is reported separately. If a later pitch increase reaches the original
end sooner than planned, the final sample is raw and `transitionTruncated()` is set.
The lifetime is never extended. With only one raw frame remaining, raw termination
wins over first-sample continuity and is explicitly flagged as truncated fallback.
These cases are tested but NOT claimed click-free.

Repeated NoteOff never restarts a bridge. Step updates apply to both active readers.
Reset clears readers and flags. Null/zero-length renders do not advance. Invalid
start requests or active reconfiguration do not alter existing state.

No allocation, I/O, logging, blocking or search occurs in this wrapper. Work per
output sample is bounded by two existing cursor reads plus one fixed-size blend
while bridging; ordinary execution uses one reader. NoteOff probes two temporary
cursor copies at the current phase. These software bounds are not MCU cycle data.

## Validation

Run `bash scripts/test_v1_loop_release.sh` for GCC, then
`CXX=clang++ SANITIZE=1 bash scripts/test_v1_loop_release.sh` for ASan/UBSan.
The script requires the compiled native probe and runs all nine Python tests
without skips. The native suites cover 240 unchanged-behavior combinations,
360 release/rate/tail combinations, a tail-limited 9-frame ramp, irregular blocks, repeated NoteOff, fractional
entry into a blend, short tails, extremes, invalid input and reset.

The Python oracle uses closed-form source phases and independently constructs
virtual values. It compares native PCM, primary phase, completion, bridge duration
and truncation exactly, at block sizes 1 and 64. The frozen original cursor's
72 production-Voice Direct parity cases and real-bank oracle are rerun separately.

Real-bank checks read full effective [start,end) PCM, including the actual tail;
they do not substitute just the loop segment. All Host candidates and prepared
604/602/62 reference geometries are checked. Their actual Zone mode is preserved
in one control; forced UntilRelease stress is labeled separately. In the frozen
GeneralUser report all 50 candidates are Continuous: forcing UntilRelease does
NOT mean those original instruments should exit their loops on NoteOff.

Only JSON metrics/hashes are uploaded. No new PCM, WAV or SF2 redistribution.
Switch-mismatch and adjacent-step diagnostics are not listening PASS criteria;
there is no assumption that every transition reduces every numerical peak.

## Remaining gates

Subjective release quality is NOT_RUN. Truncated transitions, shadow/raw timbre,
upstream envelopes/sustain, stereo, lifetime/generation, production reference and
fast paths, and ESP32-S3 deadline/headroom still require their own validation.
Host exactness and sanitizers do not establish N16R8 realtime capacity. The loop
still has the existing shortened overlap period; no pitch-preservation claim.
