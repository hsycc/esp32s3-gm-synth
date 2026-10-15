# M5 — Timestamped Realtime Event Scheduler

## Status

**In progress as a stacked branch on the M4 Voice DSP baseline.**

## Goal

Deliver MIDI/control events to the 64-frame audio renderer at deterministic sample offsets without allocation, blocking, mutexes, or audio-side sorting.

```text
MIDI / control producer
  -> absolute 44.1-kHz frame timestamp
  -> bounded SPSC queue
  -> audio block [start, start + 64)
  -> block-local frame offset 0..63
  -> NoteOn / NoteOff / CC / ProgramChange dispatch
  -> segmented Voice render
```

## Phase A — bounded queue and block scheduler

- compact `RealtimeEvent` with absolute frame timestamp
- NoteOn / NoteOff / ControlChange / ProgramChange event types
- fixed 128-event SPSC capacity
- producer `tryPush()` is nonblocking and allocation-free
- consumer `tryPeek()` / `tryPop()` are nonblocking
- only lock-free 32-bit atomics are used in the realtime queue
- producer timestamps must be nondecreasing
- equal timestamps preserve FIFO order
- future events stay queued
- events exactly at block end belong to the next block
- late events clamp to offset zero and increment telemetry
- a full queue rejects explicitly and increments telemetry

## Phase B — Voice integration

- render the block in segments between distinct event offsets
- dispatch NoteOn before rendering the following segment
- dispatch NoteOff at the exact requested frame
- define same-frame ordering policy and test it
- map ProgramChange events to the control-domain latest-wins mailbox without putting SF2 loading on Audio
- map supported realtime CCs to channel state without unbounded work

## Phase C — producer timebase

- define the monotonic render-frame clock shared by MIDI/control and Audio
- convert UART/USB/BLE MIDI arrival time to render-frame timestamps
- bound producer lookahead
- define policy for events arriving after their intended frame
- expose queue depth/high-water/drop/late counters

## Host validation

- offsets 0, 1 and 63 are exact
- block-end timestamp remains for the next block at offset 0
- future events remain queued
- late events clamp to 0 and increment `late_events`
- equal timestamps preserve FIFO
- timestamp regression is rejected
- invalid channel is rejected
- capacity overflow is explicit and recoverable
- a full-capacity same-frame batch drains without allocation
- zero-length blocks consume nothing

## ESP32-S3 / QEMU validation

- all queue atomics used by the hot path are compile-time lock-free 32-bit atomics
- Xtensa build succeeds under pinned ESP-IDF v6.1
- existing real-SF2 Program Prepare / residency gates remain green
- no new panic or stack-growth regression is introduced

## Exit criteria

- a deterministic Host test can schedule NoteOn/NoteOff within a 64-frame block and observe the correct sample offsets
- Audio does not allocate, block, take a mutex, sort an unbounded collection, or touch storage
- queue overflow and late-event behavior are explicit and measurable
- ProgramChange remains a control/storage operation rather than an audio-thread SF2 load
