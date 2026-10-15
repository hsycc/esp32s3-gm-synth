# M3 — Sample Residency Manager（采样驻留管理器）

## Status

- **Phase A complete in Host CI** — arena allocation, pin/ref lifetime, fragmentation, stale lease checks.
- **Phase B publication/snapshot complete in Host CI** — cross-core publication protocol, deferred reclamation, move-only Voice Refs, READY/commit/ack program exchange.
- **Phase B real-dependency path complete** — analyzer manifests generate real Program -> SampleId fixtures and 16-channel ownership stress tests.
- **Phase B source-backed Prepare complete** — missing samples load transactionally through `SampleSource`, directly into the arena without a sample-sized staging buffer.
- **Native SF2 sample source complete in Host CI** — real `.sf2` `RIFF/sdta/smpl/pdta/shdr` is parsed through random access and PCM is copied in 32 KiB chunks directly into the residency arena.
- **Phase B remaining** — capacity/admission policy, safe unowned-cache eviction, latest-desired Program Change coalescing, and richer prepare/load diagnostics.
- **Phase C pending** — ESP32-S3 N16R8 PSRAM arena + Flash/LittleFS random-access backend + hardware timing/telemetry.

M2.5 L0/L1 real-bank conformance is strict-green and remains the semantic baseline entering M3.

## Hardware baseline

This milestone targets **ESP32-S3 N16R8**:

- 16 MiB flash
- 8 MiB PSRAM
- 44.1 kHz / 16-bit / stereo
- 16 MIDI channels

R16/full-bank assumptions are not allowed in the core design. Full residency is an optimization only when measured data proves it fits with reserve.

## Why M3 exists

A user-facing `.sf2` can be much larger than available PSRAM. The audio task must never read flash/filesystem storage on NoteOn or inside render loops.

The implemented flow is now:

```text
native .sf2
  -> RandomAccessReader
  -> Sf2SampleSource (shdr + smpl offsets)
  -> ProgramPrepareController
  -> SampleResidencyManager::loadFromSource
  -> direct chunked write into arena / future PSRAM
  -> Program Pin
  -> PublishedSampleDirectory
  -> immutable ProgramSnapshot READY
  -> block-boundary Commit
  -> move-only PublishedVoiceRef
  -> Voice DSP
```

Control/storage owns parsing, storage I/O, allocation and loading. Audio only consumes already-published immutable records and PCM.

## Measured real-bank pressure

CI derives these values from the checked-in analyzer manifests and regenerates the C++ dependency fixture rather than hand-copying SampleIds.

### FM/GM Open ~6 MiB subset

- all referenced PCM, 4-byte aligned: `5,881,848` bytes
- greedy 16-program union: `2,221,452` bytes (~2.12 MiB)
- unique samples in greedy 16-program union: `16`
- 2 MiB PCM budget: does not fit
- 4/5/6 MiB PCM budgets: greedy 16-program union fits
- full subset sample union requires roughly a 6 MiB PCM budget

### GeneralUser GS ~6 MiB subset

- all preset sample union: `5,862,084` bytes
- greedy 16-program union: `5,675,820` bytes (~5.41 MiB)
- unique samples in greedy 16-program union: `121`
- greedy 16-program union is about 96.8% of the whole subset sample union
- 2/4/5 MiB PCM budgets: do not fit
- 6 MiB PCM budget: fits

### FluidR3_GM ~6 MiB subset

- all preset sample union: `5,887,528` bytes
- greedy 16-program union: `5,887,528` bytes (~5.61 MiB)
- unique samples in greedy union: `102`
- only the 6 MiB test budget fits this subset's greedy 16-program union

### Product implication

"Cache only the currently selected 16 programs" does **not** guarantee a small working set. A multisampled bank can make 16 MIDI channels pin almost the entire bank.

For arbitrary user SF2 files, an active 16-channel working set may fundamentally exceed the PCM arena. No allocator can solve that case without changing product behavior. V1 therefore needs an explicit admission policy: a new Program Change that cannot be prepared without violating active ownership must fail/defer while the old Active Program remains valid. Streaming/degraded loading, if desired later, is a separate feature rather than a hidden fallback.

## Architecture

### Sample identity

A `SampleId` identifies one SF2 sample header entry within the currently loaded bank generation. Multiple presets/instruments/zones referring to the same SampleId share one physical resident PCM allocation.

### Ownership classes

Resident sample lifetime is protected by independent ownership classes:

1. **Program/Channel Pin**
   - owned by prepared/active Program snapshots
   - control-side only

2. **Published Voice Ref**
   - owned by one playing/releasing Voice
   - audio-side acquire/release only
   - move-only so one ownership token cannot normally be copied/released twice

Removing a Program Pin never invalidates an already-acquired release-tail Voice Ref.

### Arena

The host implementation uses a caller-supplied contiguous arena with aligned first-fit allocation and free-span coalescing. On ESP32-S3 that arena becomes PSRAM-backed.

Current diagnostics include:
- capacity/used/high-water bytes
- resident/pinned/referenced counts
- free span count / largest free span
- allocation failures
- stale-reference faults
- descriptor conflicts

No silent compaction of live sample data is allowed.

## Direct-to-arena loading

`SampleResidencyManager::loadFromSource()` is intentionally transactional:

```text
reserve free span
  -> callback receives destination inside the arena
  -> SampleSource reads/copies directly into destination in chunks
  -> success: commit hash/Entry + generation
  -> failure: return span, SampleId remains invisible/non-resident
```

This avoids a second sample-sized staging buffer. A source may still use a small fixed storage/DMA bounce buffer if its platform driver requires one, but it must not require another complete PCM copy.

Host tests inject a read failure after a partial destination write and verify:
- SampleId never becomes resident;
- used bytes remain zero;
- free span is restored/coalesced;
- retry can reuse the same span successfully.

## Native SF2 SampleSource

Implemented:
- `firmware/storage/RandomAccessReader.h`
- `firmware/sf2/Sf2SampleSource.h/.cpp`

`Sf2SampleSource` performs random-access parsing only:

1. validate `RIFF` + `sfbk`;
2. locate `LIST sdta` -> `smpl`;
3. locate `LIST pdta` -> `shdr`;
4. read fixed 46-byte `shdr` records into bounded metadata;
5. exclude the terminal EOS `shdr` record;
6. convert `dwStart/dwEnd` 16-bit sample-point indices into source byte offsets;
7. read requested PCM in 32 KiB chunks directly to the destination arena.

The SoundFont file is never fully loaded into RAM.

Current real-file Host validation confirms:
- FM/GM subset: 67 playable sample headers;
- GeneralUser subset: 186 playable sample headers;
- FluidR3 subset: 102 playable sample headers;
- FM/GM sample 0: `127,890` PCM bytes / `63,945` frames / `44,100` Hz / root key 60;
- first and last resident PCM bytes exactly match random-access reads from the corresponding real `.sf2` `smpl` range.

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
  -> load missing PCM directly into arena
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

If a later dependency fails during Prepare, temporary Pins/Publications are rolled back. PCM that was already successfully loaded remains as an unpinned cache entry, so retry does not reread it unnecessarily.

## Realtime contract

Audio hot path MUST NOT:
- allocate/free arena memory;
- perform filesystem/flash reads;
- compact/rebuild the arena;
- block on a mutex held by storage/control code;
- scan program dependency sets during commit;
- log.

Audio may:
- load active/ready snapshot pointer/token;
- acquire an already-published sample through a bounded atomic protocol;
- increment/decrement one Voice Ref;
- read immutable metadata and PCM.

## Publication safety

The implementation does not rely on unsafe `pointer load -> ref++ -> pointer recheck` reclamation.

Audio enters a short sequentially-consistent acquire grace section. Control may reclaim an unpublished record only when:

```text
record no longer published
&& activeAcquireCriticalSections == 0
&& record.voiceRefs == 0
```

Bank generation cannot change while anything remains published or an old published Voice Ref remains alive.

## ProgramSnapshotExchange

Current fixed 16-channel exchange implements:

`Prepare/Pin -> READY -> block-boundary Commit -> committed token -> Control Ack -> retire previous pins`

Properties:
- immutable caller-owned snapshots;
- no heap allocation in exchange/commit;
- Audio commit performs pointer/token operations only;
- one in-flight transition per channel bounds rapid Program Change state;
- acknowledgement returns the exact previous Active snapshot to Control.

## Host validation currently enforced

CI covers:
- allocator/load/acquire/release/evict;
- duplicate SampleId reuse;
- pin and Voice Ref independent lifetime;
- free-span fragmentation/coalescing;
- stale/double Phase-A leases;
- direct-to-arena success/failure rollback;
- publish/acquire/unpublish/deferred reclaim;
- bank generation blocked by live old Voice Ref;
- 4 concurrent readers + 250 unpublish/reclaim/republish cycles;
- READY/Active block-boundary snapshot semantics;
- source-backed Prepare and cache reuse;
- source failure after earlier successful dependencies;
- old/new Program overlap and release tails;
- generated real-bank 16-channel dependency graphs;
- native real `.sf2` sample counts/descriptors/PCM byte equivalence.

## Phase B remaining — capacity/admission and cache eviction

Next implementation work:

1. expose/control LRU age for unpinned resident cache entries;
2. when `NoContiguousSpace`/metadata exhaustion occurs, evict only entries that are simultaneously:
   - Program Pin count = 0;
   - Phase-A ref count = 0;
   - unpublished from the audio directory;
   - safe according to deferred reclamation / published VoiceRef state;
3. retry direct-to-arena load after bounded safe eviction;
4. distinguish storage I/O failure from residency-capacity/fragmentation failure in `ProgramPrepareError` and telemetry;
5. prove a failed new Prepare never damages the previous Active Program;
6. retain/coalesce only the latest desired Program Change while one transition is in flight;
7. record load/reuse/eviction bytes and prepare latency.

No eviction may move live PCM. Fragmentation is recovered by evicting safe cache entries and coalescing spans, not by compacting active samples.

## Phase C — ESP32-S3 N16R8 backend

After the target ESP-IDF/toolchain baseline is pinned:
- allocate the arena from PSRAM (`MALLOC_CAP_SPIRAM` or equivalent abstraction);
- implement Flash/LittleFS `RandomAccessReader` for the selected bank storage path;
- optionally add SD/MMC reader later without changing `Sf2SampleSource`;
- keep hot publication/snapshot metadata in internal RAM where measurements support it;
- perform storage reads only from control/storage task;
- retain 32 KiB chunked reads unless hardware profiling suggests another size;
- add PSRAM high-water, load latency, source read bytes and eviction telemetry;
- confirm no storage calls appear in audio-task traces;
- measure atomic acquire/release and block-boundary commit cost on actual ESP32-S3.

## Exit criteria

- Phase A lifetime/fragmentation tests pass in CI;
- Phase B publication/snapshot tests pass in CI;
- native real SF2 source path passes real-file CI;
- Program/Channel Pins and Voice Refs have independent verified lifetime;
- duplicate samples are physically resident once;
- samples cannot be reclaimed while publication acquire/Voice ownership remains;
- release tails remain valid across Program Change;
- real SF2 dependency sets prepare/rollback transactionally;
- 16-channel partial-resident pressure is characterized on R8-sized budgets;
- safe cache eviction/admission preserves old Active Program on prepare failure;
- N16R8 hardware telemetry reports capacity, high-water, fragmentation, prepare failures, source bytes and published/ref counts.
