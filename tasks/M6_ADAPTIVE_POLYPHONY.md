# M6 — Adaptive Polyphony and Safe Voice Stealing

## Goal

Make overload behavior graceful and measurable instead of allowing repeated deadline misses or hard-cut clicks. M6 builds on the measured M4 audio deadline baseline and the bounded M5 event path.

## Work

- measure per-block render duration and deadline misses
- derive rolling CPU/deadline pressure
- separate the compile-time hard Voice cap from the runtime admission limit
- lower admission after true misses or sustained pressure
- recover capacity gradually after sustained stability
- add hysteresis so the admission limit does not oscillate
- add fast-release/fade victim handling for Voice stealing
- define duplicate-note and exclusive-class behavior
- optionally use per-Voice DSP complexity as a predictive admission signal
- record pressure events, steals and current admission limit

## Validation

- overload lowers new-Voice admission before repeated failure
- stable conditions restore capacity gradually
- counters do not silently overflow
- stealing avoids an abrupt PCM discontinuity
- admission never violates the M3 PublishedVoiceRef lifetime contract
- the hard Voice cap remains a safety ceiling, not a claim of always-safe realtime polyphony

## Exit criteria

The synth has a tested overload policy whose runtime Voice admission follows measured N16R8 headroom rather than assuming `MAX_VOICES` is always realtime-safe.
