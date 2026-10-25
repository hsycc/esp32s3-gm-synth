# Experimental C++ loop cursor: execution gate, not firmware enablement

Frozen Host policy baseline: `de684b82d70e8547614592fba680d53b28aa92d9`
(`prepared-loop-review-v3-source-matched-peak`, PR #20). This batch must not
change policy thresholds or existing Voice rendering. It is a stacked,
independent draft for executable transport verification.

## Implementation contract

`firmware/voice/experimental/LoopCrossfadeCursor.h` is NOT included by the
production Voice, Program Prepare or any app image. Default `ExperimentalFade`
is disabled; a caller must explicitly provide `enabled=true` and 16/32/64
**source frames**. A Host candidate does not authorize this override.

PCM is immutable mono signed PCM16 borrowed from the caller. Effective start,
end and loop coordinates must be validated; invalid starts are transactional.
No heap allocation, file access, logging or locking occurs inside the cursor.
The caller must pin PCM ownership and serialize control/render operations.
This cursor does not itself implement SampleRef acquisition or generation
validation. Reconfiguration while active is rejected; reset is explicit.

For N faded source points, integer source point k in the last N samples is:

`round((tail[k]*(N-k) + head[k]*(k+1))/(N+1))`

The denominator is odd for the permitted lengths, so integer nearest rounding
has no half-way ties and matches the existing Python audition transform.
Q32.32 phase interpolates **between these quantized virtual sample points**;
it does not first interpolate two streams and then mix them. Signed linear
interpolation uses floor, matching the existing Voice Direct path. Wrap resumes
at loopStart+N, preserving fractional overshoot. Subsequent period is L-N,
not L. This is not a pitch-preserving crossfade algorithm.

Advance uses distance-to-end subtraction before addition and one modulo for
large overshoot. This prevents uint64 addition overflow and unbounded per-wrap
loops. Variable positive phase increments are supported. Integer division and
64-bit modulo still require MCU profiling; Host timings are not S3 capacity.

## NoteOff and scope of correctness

- Continuous loops continue at the transport layer after NoteOff; the upstream
  voice envelope is responsible for eventual silence.
- UntilRelease disables looping and blending immediately at the event, keeps
  the current source phase, reads the original tail and stops at effective end.
- NoteOff before/inside/after the overlap, repeated NoteOff, one-shot ending,
  reset and irregular block boundaries are covered by tests.
- **Mid-blend release can change waveform abruptly. No click-free release is
  claimed.** This batch deliberately adds no implicit release fade/DC correction.
  A bounded exit transition must be separately evaluated before enabling this
  mode in production. Sustain routing and envelopes are not part of this cursor.

## Verification

`bash scripts/test_v1_loop_cursor.sh` builds synthetic C++ suites, an executable
comparison against the unchanged production `Voice::renderAdd` (Direct mode,
three loop modes, effective regions, fractional rates and NoteOff envelopes),
and a binary PCM probe. The Python tests invoke the compiled probe; CI supplies
it explicitly, so native tests must not be skipped in the dedicated job.

Dedicated CI runs GCC and Clang ASan/UBSan, then recomputes all eligible GeneralUser
geometries with the unchanged Host policy. Every HOST_CANDIDATE plus explicit
reference IDs is tested with Direct, unity/fractional phases, multiple block
sizes and UntilRelease. The Python oracle is also compared to the original
WAV renderer at unity rate over several wraps. JSON contains hashes/counts,
not PCM/WAV or an embedded plan. Native sample rate transport and interpolated
PCM equivalence do not establish end-to-end instrument timbre or listening PASS.

## Remaining gates

No automatic Planner-to-cursor mapping, no live Voice/fast-path hook, no linked
stereo policy, no release-transition audible approval, no PSRAM/lifetime
integration, no QEMU/ESP-IDF execution of this cursor, and no physical deadline
or polyphony claim is made here. Before production integration, validate
identity/generation and immutable publication, release transitions, fractional
pitch/timbre, then measured I2S/DMA/CPU on N16R8. Normal V1 remains Direct.
