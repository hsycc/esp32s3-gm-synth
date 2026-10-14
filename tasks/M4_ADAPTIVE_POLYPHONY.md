# M4 — Adaptive Polyphony

## Goal
Make overload behavior graceful and measurable instead of allowing repeated deadline misses or hard-cut clicks.

## Work
- measure per-block render duration
- derive rolling CPU/deadline pressure
- separate compile-time hard voice cap from runtime admission limit
- reduce admission after true deadline misses or sustained pressure
- recover gradually after sustained stability
- add hysteresis to avoid oscillation
- add fast-release/fade victim handling for voice stealing
- define duplicate-note and exclusive-class behavior

## Validation
- overload lowers new-voice admission before repeated failure
- stable conditions restore capacity gradually
- no silent counter overflow
- stealing is audibly cleaner than hard discontinuity
- metrics record steals, pressure events and current admission limit

## Exit criteria
The synth has a tested overload policy and does not equate `MAX_VOICES` with always-safe realtime capacity.