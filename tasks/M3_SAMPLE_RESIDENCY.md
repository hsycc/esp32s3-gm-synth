# M3 — Sample Residency Manager（采样驻留管理器）

## Status

Started after M2.5 L0/L1 real-bank conformance reached zero structural and generator failures.

## Hardware baseline

This milestone targets **ESP32-S3 N16R8**:

- 16 MiB flash
- 8 MiB PSRAM
- 44.1 kHz / 16-bit / stereo
- 16 MIDI channels

R16/full-bank assumptions are not allowed in the core design. Full residency is an optimization only when measured data proves it fits with reserve.

## Why M3 exists

A user-facing `.sf2` can be much larger than available PSRAM. The audio task must never read flash/filesystem storage on NoteOn or inside render loops.

Therefore the runtime needs a stable layer between SF2 sample IDs and sample PCM memory:

`SF2 sample dependency -> SampleResidencyManager -> stable SampleRef -> Voice`

The manager decides which PCM is resident; Voice DSP only consumes an already-resident handle.

## First measured reference

The comparable FM/GM Open ~6 MiB fixture already demonstrates the intended shape:

- all referenced PCM, 4-byte aligned: `5,881,848` bytes
- greedy 16-program working set: `2,221,452` bytes

On R8, even a ~6 MiB bank leaves little room for firmware/runtime/FX buffers if blindly kept fully resident. Larger GeneralUser/FluidR3-class sources require partial/shared residency.

## Architecture

### Sample identity

A `SampleId` identifies one SF2 sample header entry. Multiple presets/instruments/zones referring to the same `SampleId` must share one resident PCM allocation.

### Ownership classes

Each resident sample tracks two independent lifetime classes:

1. **Program/Channel Pin**
   - keeps samples available for prepared/current programs
   - controlled outside the audio hot path

2. **Voice Ref**
   - keeps PCM valid while a playing/releasing voice still references it
   - a Program Change may remove a pin, but release tails keep their Voice Refs

A sample is evictable only when:

`pinCount == 0 && voiceRefCount == 0`

### Stable references

A Voice receives a `SampleRef` containing:

- sample identity
- generation/token
- PCM pointer
- byte/frame length
- loop start/end
- source sample rate
- root key
- pitch correction
- sample flags/type

Resident allocations MUST NOT move while a Voice Ref exists.

### Arena

V1 uses a contiguous PSRAM arena with aligned allocations and free-span coalescing.

Required diagnostics:
- capacity bytes
- used bytes
- high-water bytes
- resident sample count
- pinned sample count
- referenced sample count
- free span count
- largest free span
- allocation failures
- stale-reference faults

No silent compaction of live sample data is allowed. Fragmentation recovery must either evict safe entries, rebuild only when no live references can be invalidated, or later migrate through an explicit double-buffered path.

## Runtime lifecycle

### Full-resident fast path

Only when analyzer + hardware reserve prove it fits:

`load all sample dependencies -> pin bank -> Program Change becomes metadata/state change`

### General partial-resident path

`Program Change request`

`-> compute required SampleId union`

`-> PrepareProgram in control/storage domain`

`-> reuse already-resident samples`

`-> load missing samples into PSRAM`

`-> mark READY`

`-> commit program at audio block boundary`

`-> retire old program pins`

`-> samples still referenced by release-tail voices remain resident`

## Realtime contract

The audio hot path MUST NOT:
- allocate/free arena memory
- perform filesystem/flash reads
- compact/rebuild the arena
- block on a mutex held by storage/control code
- log

The audio side may:
- resolve/pick an already-published resident handle
- increment/decrement the Voice Ref through a bounded non-blocking mechanism
- consume immutable SampleRef metadata and PCM

The exact cross-core publication mechanism is Phase B of this milestone; the first Host core validates allocator/lifetime semantics single-threaded.

## Phase A — platform-neutral semantic core

Implement a C++ host-testable residency core with:
- fixed metadata capacity
- deterministic aligned first-fit arena allocator
- duplicate SampleId reuse
- generation-checked `SampleRef`
- pin/unpin
- acquire/release
- safe eviction
- free-span coalescing
- fragmentation statistics
- deterministic out-of-memory/fragmentation failure

Host tests must cover:
1. basic load/acquire/release/evict
2. duplicate load reuses one allocation
3. pinned sample cannot be evicted
4. active Voice Ref survives pin removal
5. adjacent free spans coalesce
6. fragmented free bytes do not masquerade as contiguous capacity
7. stale/double release is rejected
8. high-water/stat counters are correct
9. reset/rebuild refuses to invalidate live refs

## Phase B — program dependency and publication model

Integrate analyzer/SF2 metadata concepts:
- program -> unique SampleId dependency set
- shared samples across channels/programs
- prepare token / READY snapshot
- block-boundary commit
- old-program pin retirement
- control/audio ownership handoff

Define a non-blocking publication mechanism suitable for ESP32-S3 dual-core execution before platform integration.

## Phase C — ESP32-S3 N16R8 backend

After M0 toolchain target is pinned:
- allocate arena from PSRAM (`MALLOC_CAP_SPIRAM` or equivalent platform abstraction)
- keep hot metadata/control structures in internal RAM where measurement supports it
- stream/copy sample data only from control/storage task
- add real PSRAM high-water and load-time telemetry
- confirm no storage calls appear in audio task traces

## Exit criteria

- Phase A lifetime/fragmentation tests pass in CI
- program/channel pins and Voice Refs have independent, verified lifetime
- duplicate samples are physically resident once
- samples cannot be evicted while pinned/referenced
- release tails remain valid across Program Change
- partial-resident working sets can be prepared without touching audio storage I/O
- large SF2 sources do not require full-bank residency
- N16R8 telemetry reports capacity, high-water, fragmentation and allocation failures
