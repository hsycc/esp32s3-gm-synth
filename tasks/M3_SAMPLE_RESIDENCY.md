# M3 — Sample Residency Manager（采样驻留管理器）

## Status

- **Phase A: complete in Host CI** — arena allocation, pin/ref lifetime, fragmentation, stale lease checks.
- **Phase B core: implemented and under Host CI** — published sample directory, deferred reclamation, move-only Voice Refs, program READY/commit/ack exchange, shared-pin/release-tail integration tests.
- **Phase B next:** connect real SF2 program dependency sets to prepare/rollback/retire control logic and run multi-channel residency pressure cases.
- **Phase C:** pending ESP32-S3 N16R8 backend/toolchain integration.

M2.5 L0/L1 real-bank conformance is already strict-green and is the semantic baseline entering this milestone.

## Hardware baseline

This milestone targets **ESP32-S3 N16R8**:

- 16 MiB flash
- 8 MiB PSRAM
- 44.1 kHz / 16-bit / stereo
- 16 MIDI channels

R16/full-bank assumptions are not allowed in the core design. Full residency is an optimization only when measured data proves it fits with reserve.

## Why M3 exists

A user-facing `.sf2` can be much larger than available PSRAM. The audio task must never read flash/filesystem storage on NoteOn or inside render loops.

The runtime therefore separates:

`SF2 dependency -> control-side residency arena -> PublishedSampleDirectory -> move-only Voice Ref -> Voice DSP`

Control/storage owns allocation and loading. Audio only consumes already-published immutable records and PCM.

## First measured reference

The comparable FM/GM Open ~6 MiB fixture already demonstrates the intended shape:

- all referenced PCM, 4-byte aligned: `5,881,848` bytes
- greedy 16-program working set: `2,221,452` bytes

On R8, even a ~6 MiB bank leaves little room for firmware/runtime/FX buffers if blindly kept fully resident. Larger GeneralUser/FluidR3-class sources require partial/shared residency.

## Architecture

### Sample identity

A `SampleId` identifies one SF2 sample header entry within the currently loaded bank generation. Multiple presets/instruments/zones referring to the same SampleId share one physical resident PCM allocation.

### Ownership classes

Resident sample lifetime is protected by independent ownership classes:

1. **Program/Channel Pin**
   - owned by prepared/active program snapshots
   - control-side only

2. **Published Voice Ref**
   - owned by one playing/releasing Voice
   - audio-side acquire/release only
   - move-only so one ownership token cannot be copied normally

Removing a Program Pin never invalidates an already-acquired release-tail Voice Ref.

### Arena

Phase A uses a caller-supplied contiguous arena with aligned first-fit allocation and free-span coalescing. On ESP32-S3 that arena will be PSRAM-backed.

Current diagnostics include:
- capacity/used/high-water bytes
- resident/pinned/referenced counts
- free span count / largest free span
- allocation failures
- stale-reference faults
- descriptor conflicts

No silent compaction of live sample data is allowed.

## Runtime lifecycle

### Full-resident fast path

Only when analyzer + hardware reserve prove it fits:

`load all dependencies -> pin bank/program set -> Program Change becomes metadata/state change`

### General partial-resident path

```text
Program Change request
  -> resolve unique SampleId dependencies
  -> Prepare in control/storage domain
  -> reuse resident/published samples
  -> load missing PCM privately
  -> publish complete immutable ResidentRecord
  -> acquire Program Pins
  -> publish READY ProgramSnapshot
  -> Audio commits at block boundary
  -> Control observes commit acknowledgement
  -> retire old Program Pins
  -> old release-tail Voice Refs keep retired samples alive
  -> unpublish unpinned cache candidates
  -> reclaim only after publication grace period + VoiceRefs drain
```

## Realtime contract

Audio hot path MUST NOT:
- allocate/free arena memory
- perform filesystem/flash reads
- compact/rebuild the arena
- block on a mutex held by storage/control code
- scan program dependency sets during commit
- log

Audio may:
- load active/ready snapshot pointer/token
- acquire an already-published sample through a bounded atomic protocol
- increment/decrement one Voice Ref
- read immutable metadata and PCM

## Phase A — platform-neutral semantic core

Implemented in:

- `firmware/sample/SampleResidencyManager.h/.cpp`
- `tests/sample/test_sample_residency.cpp`

CI covers:
1. basic load/acquire/release/evict
2. duplicate SampleId reuse
3. pinned sample cannot be evicted
4. active Voice Ref survives pin removal
5. adjacent free-span coalescing
6. fragmented total free bytes are not mistaken for contiguous capacity
7. stale/double Phase-A lease release rejection
8. high-water/alignment counters
9. reset refuses to invalidate live owners
10. bounded Phase-A voice lease capacity

## Phase B — publication and program snapshot core

Implemented in:

- `firmware/sample/PublishedSampleDirectory.h/.cpp`
- `firmware/sample/ProgramSnapshot.h/.cpp`
- `tests/sample/test_sample_publication.cpp`
- `docs/SAMPLE_RESIDENCY_DESIGN.md`

### PublishedSampleDirectory

Current protocol:

- complete `ResidentRecord` is published atomically only after PCM/metadata are ready
- Audio enters a short sequentially-consistent acquire grace section before reading the published pointer
- successful acquire increments the exact record's `voiceRefs`
- `PublishedVoiceRef` is move-only and invalidated on release
- Control unpublishes first; Audio can no longer create new refs to the retired record
- Control may reclaim only when:

```text
record no longer published
&& activeAcquireCriticalSections == 0
&& record.voiceRefs == 0
```

- bank generation cannot change while anything remains published or any old published Voice Ref is live

This closes the use-after-free window that a plain `pointer load -> refs++ -> pointer recheck` scheme would leave.

### ProgramSnapshotExchange

Current 16-channel exchange implements:

`Prepare/Pin -> READY -> block-boundary Commit -> committed token -> Control Ack -> retire previous pins`

Properties:
- snapshots are immutable caller-owned objects
- no heap allocation in exchange/commit
- Audio commit does pointer/token operations only
- one in-flight transition per channel bounds rapid Program Change state
- acknowledgement returns the exact previous Active snapshot to Control

### Phase-B Host validation

Current tests cover:
- publish/acquire/unpublish/deferred reclaim
- bank-generation block while old Voice Ref exists
- explicit publish conflict handling
- 4 reader threads + 250 Control unpublish/reclaim/republish cycles
- READY visible while old Active remains unchanged before block commit
- block-boundary Active swap
- wrong commit token rejection
- rapid second READY rejected until first transition is acknowledged
- old/new program dependency overlap (`{1,2}` -> `{2,3}`)
- shared sample stays pinned by the new program after old pin retirement
- old-program-only release-tail sample stays resident until Voice release

## Phase B next — real dependency preparation

Next implementation step:

1. expose a control-only resident sample view from the arena layer (pointer + descriptor + sample generation);
2. build fixed-capacity `ResidentRecord` storage tied to the arena entries;
3. feed actual analyzer/ResolvedZone `program -> unique SampleId` sets into a Prepare controller;
4. transactionally pin/publish all required samples or roll back on failure;
5. retain only the latest desired Program Change while one transition is in flight;
6. stress 16 MIDI channels with overlapping and non-overlapping real-bank program working sets;
7. report bytes loaded vs reused, high-water, fragmentation and prepare failures.

## Phase C — ESP32-S3 N16R8 backend

After the toolchain target is pinned:
- allocate arena from PSRAM (`MALLOC_CAP_SPIRAM` or equivalent platform abstraction)
- keep hot publication/snapshot metadata in internal RAM where measurements support it
- copy/read sample data only from control/storage task
- add PSRAM high-water/load-time telemetry
- confirm no storage calls appear in audio-task traces
- measure atomic acquire/commit cost on actual ESP32-S3

## Exit criteria

- Phase A lifetime/fragmentation tests pass in CI
- Phase B publication/snapshot tests pass in CI
- program/channel pins and Voice Refs have independent verified lifetime
- duplicate samples are physically resident once
- samples cannot be reclaimed while published acquire/Voice ownership remains
- release tails remain valid across Program Change
- real SF2 dependency sets can be prepared/rolled back transactionally
- 16-channel partial-resident pressure cases are characterized on R8-sized budgets
- N16R8 telemetry reports capacity, high-water, fragmentation, prepare failures and published/ref counts
