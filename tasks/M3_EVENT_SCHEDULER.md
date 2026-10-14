# Legacy filename — Event Scheduler is now M5

> This file is retained to avoid breaking old links. The durable implementation
> plan moved Sample Residency ahead of event scheduling. Treat this work as
> **M5 — Timestamped Realtime Event Scheduler**. Current M3 work is documented
> in `tasks/M3_SAMPLE_RESIDENCY.md`.

## Goal
Implement a timestamped bounded musical-event path with intra-block execution.

## Target flow
`input -> MIDI/internal decoder -> timestamp -> bounded queue -> block scheduler -> frame-offset event execution`

## Work
- define compact internal event structure
- implement fixed-capacity queue
- define overflow behavior and counters
- convert timestamps to block-local frame offsets
- render spans between events inside one block
- keep producer/control work outside audio hot path
- add tests for event ordering and boundary cases

## Validation
- multiple Note/CC events in one block execute at distinct frame offsets
- no heap allocation in queue push/pop
- audio task never blocks on producers
- queue high-water/drop counters are observable

## Exit criteria
Event timing is no longer quantized exclusively to audio-block boundaries.
