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
                              acquire-load/recheck
                                     |
                                     v
                               Audio / Voice Core
```

The PCM allocation is private until the publication step. A partially written sample is never addressable through the audio-side directory.

## Bank scope

A SoundFont `SampleId` is local to one loaded SF2 bank. V1 treats the residency directory as scoped to the currently loaded SoundFont. Loading/replacing the whole bank creates a new bank generation; references from an older bank generation cannot match a new slot accidentally.

If multi-bank simultaneous playback is added later, identity becomes `(BankGeneration, SampleId)` rather than weakening this invariant.

## Published slot

Conceptually each parsed SF2 sample owns one directory slot:

```cpp
struct PublishedSampleSlot {
    atomic<const ResidentRecord*> published;
    atomic<uint32_t> voiceRefs;
    uint32_t sampleId;
};

struct ResidentRecord {
    uint32_t bankGeneration;
    uint32_t sampleGeneration;
    const int16_t* pcm;
    uint32_t pcmBytes;
    uint32_t frames;
    uint32_t loopStart;
    uint32_t loopEnd;
    uint32_t sampleRate;
    uint8_t rootKey;
    int8_t pitchCorrection;
    uint16_t flags;
};
```

`ResidentRecord` is immutable after publication.

The exact field packing may be optimized later, but immutability is part of the contract.

## Publish protocol

Control/storage domain:

1. Resolve SampleId dependency.
2. Reuse an already-published compatible record when present.
3. Otherwise reserve aligned PSRAM from the control-owned arena.
4. Read/copy the complete PCM payload into that private allocation.
5. Fill the complete immutable `ResidentRecord`.
6. Publish the record pointer with **release** semantics.
7. Only after publication may a READY program snapshot refer to that sample.

Publication of a null/partial record is forbidden.

## Audio acquire protocol

Audio/Voice domain performs a bounded operation; it does not allocate arena memory or touch storage.

Conceptual loop:

```text
record1 = slot.published.load(acquire)
if null: acquire fails

voiceRefs++

record2 = slot.published.load(acquire)
if record2 == record1:
    SampleRef succeeds
else:
    voiceRefs--
    retry/fail boundedly
```

The second read closes the race where control unpublishes between the first pointer load and the reference increment.

A production implementation may use a generation/tagged-pointer variant, but it must preserve the same safety property.

## Audio release protocol

A `SampleRef` contains enough identity to reject stale releases:

- bank generation
- sample generation
- SampleId/slot index
- resident record pointer/token

Release decrements only the reference count associated with the exact published/retiring generation it acquired.

Audio release never frees PSRAM.

## Unpublish / eviction protocol

Control domain may consider a sample for eviction only when its program/prepared pin count is zero.

Eviction is two-stage:

1. Atomically replace `slot.published` with null using acquire-release semantics.
2. Mark the old `ResidentRecord`/allocation as **retiring**.

After unpublish:
- no new Voice may acquire that record;
- existing Voice Refs remain valid;
- the PSRAM allocation must not be returned to the free arena until its reference count reaches zero.

This is effectively deferred reclamation. The control task periodically collects retiring records whose ref count is zero.

## Why `refs == 0` check before unpublish is not sufficient

This sequence is unsafe:

```text
control: sees refs == 0
                        audio: reads published pointer
control: frees PCM
                        audio: refs++ and uses stale pointer
```

Therefore publication must be removed first, and reclamation happens only after the post-unpublish reference count is known to be zero.

## Program residency snapshots

Each MIDI channel logically has:

- `ActiveProgramSnapshot`
- at most one `Pending/ReadyProgramSnapshot`
- a short `RetiringProgramSnapshot` handoff until control observes commit acknowledgement

A snapshot is immutable and contains at minimum:

```text
channel
bank/program
program generation/token
ResolvedZone/Program metadata reference
unique required SampleId set
```

### Prepare

Control domain:

1. Resolve unique SampleId dependency set for requested program.
2. Ensure every required sample is published/resident.
3. Increment program pins for the new dependency set.
4. Build immutable READY snapshot.
5. Publish only the READY snapshot/token to audio.

Failure at any step rolls back the new snapshot's pins. Successfully loaded but now-unpinned samples may remain as evictable cache entries.

### Commit

At an audio block boundary:

1. Observe READY snapshot.
2. Atomically/selectively replace the channel's Active snapshot.
3. Emit commit acknowledgement/token for control.
4. Continue rendering; no storage/arena work occurs here.

### Retire

After control observes the commit acknowledgement:

1. Retire pins owned by the previous Active snapshot.
2. Samples still used by release-tail Voice Refs remain alive automatically.
3. Unpinned + unreferenced cache entries become eviction candidates.

## Rapid Program Change policy

V1 allows at most one published READY transition per channel. While a transition awaits block-boundary commit/ack, a newer desired Program Change is retained as the **latest desired program** and prepared after the in-flight transition settles.

A not-yet-published preparation may be cancelled and its pins rolled back.

This creates bounded state and prevents an unbounded chain of residency snapshots during rapid MIDI Program Changes.

## Pin semantics

Program pins belong to control-domain snapshots, not Voices.

For one SampleId shared by several channels/programs:

```text
pinCount = number of active/prepared snapshot owners
```

One physical PCM allocation is shared. Removing one channel's pin does not evict the sample while any other snapshot or Voice still owns it.

## Arena ownership

Only the control/storage domain mutates:

- free-span list
- allocation/eviction policy
- LRU/cache policy
- PSRAM allocation metadata
- retiring allocation list

The audio domain never walks these structures.

This means the Phase-A combined semantic class will be refactored into an arena/control portion and a publication/ref portion before final ESP32 cross-core use. Phase-A tests remain valuable because pin/ref/eviction invariants must still hold after the split.

## Realtime operations allowed on audio core

Bounded operations only:

- load active/pending snapshot pointer/token
- published-slot acquire
- atomic voice ref increment/decrement
- immutable metadata reads
- PCM reads

Forbidden:

- filesystem/flash reads
- malloc/free
- arena allocation/coalescing
- eviction scans
- program dependency construction
- logging
- waiting on control task or mutex

## Diagnostics

Control-side:
- PSRAM arena capacity/used/high-water
- largest free span
- free span count
- resident/cache/retiring sample counts
- program prepare bytes loaded/reused
- prepare latency
- allocation/fragmentation failures

Audio-side:
- published acquire misses
- bounded acquire retry/failure count
- stale generation faults
- active Voice Refs

Program-change:
- prepare token
- READY timestamp
- commit block/frame
- retire acknowledgement
- superseded request count

## Correctness invariants

The implementation and tests must prove:

1. **No partial publication** — Audio cannot observe PCM before copy + metadata completion.
2. **No reuse while referenced** — PSRAM is not returned/reused until old Voice Refs reach zero after unpublish.
3. **No audio-side reclamation** — Audio never frees/evicts/coalesces.
4. **Snapshot atomicity** — one block sees either the old or new immutable program snapshot, never a partially prepared program.
5. **Independent lifetimes** — Program pins may disappear while release-tail Voice Refs keep PCM valid.
6. **Shared residency** — multiple programs/channels can pin one physical sample allocation.
7. **Bounded realtime work** — audio acquire/release has a fixed upper work bound and performs no dynamic allocation.
