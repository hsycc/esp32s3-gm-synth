# Realtime Design

## Intent

The firmware should behave as a bounded realtime audio system. Musical/control work must be separated from the audio deadline whenever possible.

## Initial task model

A practical starting point on dual-core ESP32-S3:

```text
Core A / control-oriented
- MIDI input/decoder
- asset preparation
- control-rate updates
- diagnostics aggregation
- UI/other non-audio work if later added

Core B / audio-oriented
- audio render task
- event dequeue/scheduling
- voice DSP
- mixer/FX
- I2S buffer service
```

Exact core assignment and priorities must be verified experimentally; this is a design baseline, not a permanent rule.

## Event path

Target path:

```text
transport input
    ↓
decode MIDI/internal event
    ↓
attach audio-time timestamp
    ↓
push bounded queue
    ↓
audio task snapshots events for current block
    ↓
convert to frame offsets
    ↓
render spans between events
```

Example for a 64-frame block:

```text
frames 0..17   render
frame 18       execute Note On
frames 18..42  render
frame 43       execute CC/Note event
frames 43..63  render
```

This avoids quantizing all musical events to block boundaries.

## Queue requirements

Preferred properties:
- fixed capacity
- no heap allocation in producer/consumer operation
- SPSC where topology permits
- explicit overflow counter
- defined policy for overflow; never silently corrupt state

Multiple producers should preferably merge through a control-domain stage rather than introducing complex blocking synchronization into the audio task.

## Audio task rules

The render task must:
- never wait for filesystem/storage
- never parse assets
- never print
- never allocate from general heap
- avoid blocking mutex acquisition
- bound event processing per block
- record lightweight diagnostics

## Control-rate work

Move expensive operations away from per-sample execution when they change slowly:
- pitch factor updates where approximation is acceptable
- LFO/control coefficients
- filter coefficients
- FX coefficients
- gain/pan transforms
- residency/refcount bookkeeping

Block-rate caching is encouraged when parameters are invariant for the full block.

## Deadline monitor

For each block:
1. capture start cycle/time
2. render block
3. capture end cycle/time
4. update bounded counters/high-water values

Do not format statistics inside the audio task.

The monitor should expose enough information for adaptive polyphony:
- current/rolling render pressure
- recent deadline misses
- peak render time

## Adaptive admission

Compile-time `MAX_VOICES` is a hard memory/resource cap.

A separate runtime admission limit controls whether new voices may be started.

Example behavior:
- immediate or aggressive reduction after a true deadline miss
- gentler reduction on sustained high CPU pressure
- minimum safety floor chosen from measurements
- gradual recovery after several seconds without pressure
- hysteresis to avoid oscillation

Reducing the admission limit does not require instantly killing every voice above the new limit; existing release tails may continue if the deadline budget remains safe.

## Voice stealing

Avoid abrupt sample discontinuity where practical.

Preferred behavior:
- select victim by defined score/policy
- transition victim into very short fast-release/fade state
- reserve a small tail/sacrifice capacity if required by implementation
- start new voice without blocking

Measure the CPU and memory impact of tail voices before fixing the policy.

## Asset synchronization

Audio must never observe freed or partially rebuilt sample data.

Preferred approaches:
- immutable prepared objects + pointer/index swap at a block boundary
- reference-counted sample handles with ownership rules
- epoch/generation schemes where useful

Avoid broad locks around the render loop.

## Diagnostics

Realtime-side counters should be atomic or otherwise safe with minimal overhead. Human-readable reporting must run outside the audio hot path.
