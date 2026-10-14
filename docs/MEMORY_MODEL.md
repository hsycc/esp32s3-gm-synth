# Memory Model

## Goal

Use ESP32-S3 N16R8 memory deliberately. The architecture must work with **8 MB PSRAM** and must not depend on 16 MB PSRAM.

The two current candidate banks prove that one fixed residency strategy is insufficient:
- compact banks may support full residency
- larger banks require shared/per-program residency

## Fixed memory budget

Hardware baseline:
- flash: 16 MB
- PSRAM: 8 MB

The full 8 MB PSRAM is never assumed available for PCM. Runtime reserve must cover firmware PSRAM use, allocator overhead, effects, metadata and safety margin.

## Memory classes

### Internal SRAM

Prefer for latency-sensitive small structures:
- audio DMA-capable buffers
- audio block working buffers
- hot voice metadata when measurements justify it
- compact event queue
- small FX state/delay memory when it materially reduces render latency
- task stacks and RTOS structures as required by platform

Internal SRAM is scarce. Do not move large buffers into it merely because it is faster; measure the benefit.

### PSRAM

Prefer for large resident data:
- PCM sample arena
- larger non-DMA sample buffers
- large effect delay buffers when acceptable
- parsed/compiled metadata only when access cost remains safe

### Flash/filesystem

Owns persistent product assets.

N16 capacity is also a hard product constraint: the raw source SoundFont need not be the deployed asset. Large source banks should be compiled/pruned into product-specific assets before firmware deployment.

V1 Note On must not depend on realtime reads from persistent storage.

## Measured candidate profiles

See `SF2_BANK_ANALYSIS.md` for full details.

### TimGM6mb
- aligned PCM across the complete source: about 5.47 MiB
- gross R8 headroom after complete PCM residency: about 2.53 MiB before all other runtime use
- therefore full residency is plausible but must be proven on hardware

### GMBK5X128
- complete aligned PCM: about 14.84 MiB — impossible on R8
- GM bank 0 + standard drum sample union: about 8.49 MiB — still too large for full residency
- measured 15 melodic + standard drum 16-channel union: about 5.44 MiB
- therefore shared/per-program residency is the intended runtime path

## Sample residency decision

Do not infer residency from `.sf2` file size.

Important values:
- `smpl` chunk payload size
- aligned PCM size
- sample count
- metadata overhead
- application PSRAM reserve
- sample-pool index/entry overhead
- fragmentation/allocator reserve
- active 16-channel union size

### Option A — full compiled-bank residency

Allowed only when:

`all required PCM + metadata + runtime PSRAM use + safety reserve <= 8 MB PSRAM`

Benefits:
- Program Change can be index/state only
- Note On has no asset I/O
- no runtime sample fragmentation caused by program switching
- simplest timing model

This is an optimization for small compiled banks, not the general architecture.

### Option B — shared/per-program residency

**Required V1 capability.**

Load samples required by active programs/channels and share duplicate sample references.

Benefits:
- supports source/compiled banks larger than R8
- retains storage-free Note On after preparation
- real GMBK 16-channel working sets appear feasible on R8

Costs:
- residency manager complexity
- Program Change preparation latency
- fragmentation/rebuild policy
- explicit ownership/refcount rules

### Option C — streaming

Not currently required for the two analyzed GM candidates.

Keep interfaces extensible enough for future large instruments. Streaming introduces:
- storage latency
- refill buffers
- underrun management
- fragmentation/card-quality dependencies
- source-rate-dependent bandwidth pressure

## Product asset pipeline

Preferred direction:

```text
Source SF2
   ↓
Host analyzer/compiler
   ├─ select GM banks/presets
   ├─ remove unreferenced samples
   ├─ normalize metadata
   ├─ precompute dependency sets
   └─ optional measured resampling
   ↓
Compiled product bank in N16 flash
   ↓
Residency Manager
   ↓
R8 PSRAM SamplePool
   ↓
Voice Engine
```

For `GMBK5X128`, the source file contains many non-GM variation banks. Shipping the entire 15 MiB source in N16 is not the intended product path.

## Sample handle model

The voice layer should not own raw filesystem concepts.

Conceptual interface:

```text
SampleHandle
- stable identity
- PCM/sample access information
- frame length
- loop points
- source sample rate
- root key / pitch correction
- channel/stereo metadata as needed
```

Residency/source implementations may eventually include:
- ResidentSampleSource
- StreamSampleSource

The voice engine should depend on a realtime-safe sample access contract rather than a specific storage backend.

## Lifetime

Sample data must remain valid for every active voice that references it.

Possible ownership layers:
- program/channel residency pin
- active voice reference

Prepared program state must never free PCM still referenced by an old release tail.

## Allocation strategy

For large PCM residency, prefer a dedicated PSRAM arena/pool over many unrelated heap allocations.

Desired properties:
- predictable alignment
- inspectable capacity/usage
- bounded metadata
- coalescing or rebuild strategy
- explicit entry-count limit
- diagnostics for fragmentation and largest free span

## Measurements

Record at minimum on N16R8:
- total/free internal SRAM
- largest free internal block
- total/free PSRAM
- largest free PSRAM block
- sample arena total/used/free
- resident sample count
- allocation failures
- fragmentation/repack events
- peak active-program union size

Memory design changes should include before/after measurements on hardware.
