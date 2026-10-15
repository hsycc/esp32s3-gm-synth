# Sample Residency / Publication Design

This document defines the M3 control/audio ownership boundary for ESP32-S3 N16R8.

## Scope

`SampleResidencyManager` Phase A validates allocator and lifetime semantics in a single-threaded host model. The final dual-core runtime must **not** allow the audio core to traverse allocator/hash metadata while the control/storage core mutates it.

The runtime therefore separates two responsibilities:

1. **Residency arena / loader** — control/storage domain only.
2. **Published Sample Directory** — bounded lock-free read/acquire surface for the audio domain.

## Core model

```text
Storage / Control Core

SF2 dependency set
      |
      v
PSRAM arena reserve
      |
      v
copy/read PCM into private allocation
      |
      v
fill immutable sample metadata
      |
      v
release-publish ResidentRecord
      |
      +------------------------------+
                                     |
                                     v
                            Published Sample Slot
                                     |
                              acquire / VoiceRef
                                     |
                                     v
                               Audio / Voice Core
```

The PCM allocation is private until the publication step. A partially written sample is never addressable through the audio-side directory.

## Bank scope

A SoundFont `SampleId` is local to one loaded SF2 bank. V1 treats the residency directory as scoped to the currently loaded SoundFont. Loading/replacing the whole bank creates a new bank generation; references from an older bank generation cannot match a new slot accidentally.

Bank generation may change only when:
- no sample is published;
- no publication acquire critical section is in flight;
- no live published Voice Ref from the previous bank remains.

If multi-bank simultaneous playback is added later, identity becomes `(BankGeneration, SampleId)` rather than weakening this invariant.

## Published slot

Conceptually each parsed SF2 sample owns one directory slot:

```cpp
struct PublishedSampleSlot {
    atomic<const ResidentRecord*> published;
};

struct ResidentRecord {
    uint32_t bankGeneration;
    uint32_t sampleGeneration;
    const int16_t* pcm;
    SampleDescriptor descriptor;
    atomic<uint32_t> voiceRefs;
};
```

`ResidentRecord` metadata and PCM are immutable after publication. Only `voiceRefs` changes.

## Publish protocol

Control/storage domain:

1. Resolve SampleId dependency.
2. Reuse an already-published compatible record when present.
3. Otherwise reserve aligned PSRAM from the control-owned arena.
4. Read/copy the complete PCM payload into that private allocation.
5. Fill the complete immutable `ResidentRecord`.
6. Publish the record pointer atomically.
7. Only after publication may a READY program snapshot refer to that sample.

Publication of a null/partial record is forbidden. Replacing a live slot directly is also forbidden; replacement requires explicit unpublish followed by deferred reclamation.

## Why pointer-load + refcount alone is not sufficient

This commonly suggested sequence is still unsafe:

```text
audio:   record = published.load()
control: published = nullptr
control: sees record.refs == 0
control: frees record/PCM

audio:   record.refs++   // use-after-free
```

A second pointer check after `refs++` does not repair the fact that the record may already have been reclaimed before the increment.

Therefore the implementation adds an **acquire grace-period guard** around the short publication-acquire critical section.

## Audio acquire protocol

Implemented Phase-B model:

```text
activeAcquirers++        (seq_cst)
record1 = published      (seq_cst)
if null -> leave

record1.voiceRefs++
record2 = published      (seq_cst)

if record2 == record1 and bank generation still matches:
    totalLiveVoiceRefs++
    activeAcquirers--    (seq_cst)
    return move-only PublishedVoiceRef
else:
    record1.voiceRefs--
    bounded retry/fail

activeAcquirers--        (seq_cst)
```

The publication pointer and acquire-grace counter use sequentially-consistent ordering intentionally. This provides a simple ordering argument:

- if Audio enters the acquire critical section before Control's reclaim check, Control observes an active acquirer and cannot reclaim;
- if Control unpublishes and reaches the reclaim check first, a later Audio acquire can only see null/new publication, not the retired record.

The work is bounded and occurs when a Voice acquires a sample, not once per rendered audio sample.

`PublishedVoiceRef` is move-only and becomes invalid after successful release, preventing ordinary code paths from duplicating one ownership token and releasing it twice.

## Audio release protocol

A `PublishedVoiceRef` carries:

- bank generation;
- sample generation;
- SampleId;
- exact `ResidentRecord*`.

Release validates identity, decrements that exact record's `voiceRefs`, decrements the directory-wide live Voice Ref count, and invalidates the move-only ref object.

Audio release never frees PSRAM.

## Unpublish / eviction protocol

Control domain may consider a sample for unpublish only when its program/prepared pin count is zero.

Eviction is two-stage:

1. Atomically replace `slot.published` with null.
2. Mark the old `ResidentRecord`/allocation as **retiring**.

After unpublish:
- no new Voice may acquire that record;
- existing Voice Refs remain valid;
- the PSRAM allocation and record storage must not be returned/reused until `canReclaim(record)` is true.

Current `canReclaim(record)` requires:

```text
record is no longer published
&& activeAcquirers == 0
&& record.voiceRefs == 0
```

This is deferred reclamation. The control task polls/collects retiring records; Audio never waits for reclamation.

## Program residency snapshots

Each MIDI channel logically has:

- `ActiveProgramSnapshot`;
- at most one `ReadyProgramSnapshot`;
- one temporary retired snapshot held until Control acknowledges the commit.

A snapshot is immutable and contains at minimum:

```text
channel
bank/program
program token
audio-bank generation
unique required SampleId set
```

The current host implementation uses a fixed dependency array and no dynamic allocation.

### Prepare

Control domain:

1. Resolve unique SampleId dependency set for requested program.
2. Ensure every required sample is resident/published.
3. Increment program pins for the new dependency set.
4. Build immutable READY snapshot.
5. Publish only the READY snapshot to the audio exchange.

Failure at any step rolls back the new snapshot's pins. Successfully loaded but now-unpinned samples may remain as evictable cache entries.

### Commit

At an audio block boundary:

1. Atomically take the READY snapshot.
2. Atomically replace the channel's Active snapshot.
3. Preserve the previous Active pointer as `retired`.
4. Publish the committed token for Control acknowledgement.

No dependency scan, sample load, pin/unpin, arena operation, filesystem I/O or mutex occurs here.

### Retire

After Control observes and acknowledges the committed token:

1. Receive the exact previous Active snapshot pointer.
2. Retire pins owned by that snapshot.
3. Samples still used by release-tail Voice Refs remain alive automatically.
4. Shared samples remain pinned if another active/prepared snapshot still owns them.
5. Unpinned sample records may be unpublished and later reclaimed after the grace/ref conditions pass.

## Rapid Program Change policy

V1 allows at most one READY/commit/ack transition per channel. While a transition is in flight, another `publishReady()` is rejected.

The higher-level control state will retain only the **latest desired program** and prepare it after the current transition is acknowledged. A not-yet-published preparation may be cancelled and its pins rolled back.

This keeps state bounded and prevents an unbounded retiring chain during rapid MIDI Program Change traffic.

## Pin semantics

Program pins belong to control-domain snapshots, not Voices.

For one SampleId shared by several channels/programs:

```text
pinCount = number of active/prepared snapshot owners
```

One physical PCM allocation is shared. Removing one channel's pin does not evict the sample while any other snapshot or Voice still owns it.

## Arena ownership

Only the control/storage domain mutates:

- free-span list;
- allocation/eviction policy;
- LRU/cache policy;
- PSRAM allocation metadata;
- retiring allocation list.

The audio domain never walks these structures.

## Realtime operations allowed on audio core

Bounded operations only:

- load active/ready snapshot pointer/token;
- published-slot acquire;
- atomic Voice Ref increment/decrement;
- immutable metadata reads;
- PCM reads.

Forbidden:

- filesystem/flash reads;
- malloc/free;
- arena allocation/coalescing;
- eviction scans;
- program dependency construction;
- logging;
- waiting on control task or mutex.

## Host validation now implemented

Phase-B host tests cover:

- publish -> acquire -> unpublish -> deferred reclaim;
- bank generation blocked while old Voice Refs exist;
- explicit publish conflict handling;
- move-only Voice Ref invalidation on release;
- 4 concurrent reader threads with 250 control-side unpublish/reclaim/republish cycles;
- READY visible before commit while Active remains old;
- block-boundary Active swap;
- one in-flight transition per channel;
- commit acknowledgement returns the exact retired snapshot;
- shared SampleId pin counts across old/new programs;
- release-tail Voice Ref keeps an old-program-only sample resident after pin retirement.

## Diagnostics

Control-side:
- PSRAM arena capacity/used/high-water;
- largest free span;
- free span count;
- resident/cache/retiring sample counts;
- program prepare bytes loaded/reused;
- prepare latency;
- allocation/fragmentation failures.

Publication/audio-side:
- published sample count;
- active acquire critical sections;
- live Voice Refs;
- acquire successes/misses/retries;
- stale-release faults;
- publish conflicts.

Program-change:
- READY publish/rejection count;
- committed token;
- commit count;
- acknowledgement count;
- later: READY timestamp, commit block/frame and superseded desired-request count.

## Correctness invariants

The implementation and tests must preserve:

1. **No partial publication** — Audio cannot observe PCM before copy + metadata completion.
2. **Grace before reclaim** — after unpublish, retired storage is not reused until acquire critical sections and old Voice Refs are both drained.
3. **No audio-side reclamation** — Audio never frees/evicts/coalesces.
4. **Snapshot atomicity** — one block sees either the old or new immutable program snapshot, never a partially prepared program.
5. **Independent lifetimes** — Program pins may disappear while release-tail Voice Refs keep PCM valid.
6. **Shared residency** — multiple programs/channels can pin one physical sample allocation.
7. **Bounded realtime work** — audio acquire/release has a fixed upper work bound and performs no dynamic allocation.
8. **Bank-generation isolation** — a new bank identity is not installed while old published Voice Refs remain.
