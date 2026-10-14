# M3 — Event Scheduler

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