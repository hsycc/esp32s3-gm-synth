# M5 — Program Change / Asset Residency

## Goal
Make Program Change safe and realtime-friendly.

## Decision first
Use M1 data to choose:
- full fixed-bank residency, or
- shared/per-program residency.

## Preferred state machine
`REQUESTED -> PREPARING -> READY -> COMMIT -> RETIRE_OLD`

## Work
- resolve required preset/zone/sample dependencies outside audio task
- prepare/load required assets outside audio task
- use immutable/prepared runtime state where practical
- commit pointer/index state at a safe audio block boundary
- preserve sample lifetime for old release tails
- measure preparation time and audible transition behavior

## Full-resident optimization
If the complete fixed bank safely fits PSRAM:
- load once at startup
- Program Change should become state/index switching
- eliminate runtime PCM churn caused by program switching

## Validation
- no filesystem/flash reads in audio hot path
- no freed sample can be referenced by an active voice
- repeated rapid Program Change does not corrupt sample state
- transition latency/gaps are measured

## Exit criteria
Program Change no longer requires synchronous asset work inside the realtime render path.