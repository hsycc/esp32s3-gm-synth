# Memory Model

## Goal

Use ESP32-S3 internal SRAM and PSRAM deliberately. The fixed GM bank should be analyzed before choosing full residency, per-program residency, or any streaming mechanism.

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
- parsed metadata only when access cost remains safe

### Flash/filesystem/SD

Owns persistent assets. V1 Note On must not depend on realtime reads from this storage.

## Sample residency decision

M1 must calculate actual bank characteristics.

Do not infer PCM residency directly from `.sf2` file size.

Important values:
- `smpl` chunk payload size
- aligned PCM size
- sample count
- metadata overhead
- sample-pool index/entry overhead
- application PSRAM reserve
- fragmentation/allocator reserve

### Option A — full fixed-bank residency

Preferred when:

`all required PCM + metadata + runtime PSRAM use + safety reserve <= available PSRAM`

Benefits:
- Program Change can be index/state only
- Note On has no asset I/O
- no runtime sample fragmentation caused by program switching
- simplest timing model

Risks:
- leaves less PSRAM for future features/effects
- startup loading time
- must preserve adequate safety reserve

### Option B — shared/per-program residency

Load only samples required by active programs/channels and share duplicate sample references.

Benefits:
- supports banks larger than available PSRAM
- retains storage-free Note On after preparation

Costs:
- residency manager complexity
- Program Change preparation latency
- fragmentation/rebuild policy
- multi-channel working-set analysis becomes important

### Option C — streaming

Not a V1 requirement for a ~14 MB bank.

Keep the interfaces extensible enough for future large instruments. Streaming introduces:
- storage latency
- refill buffers
- underrun management
- fragmentation/card-quality dependencies
- source-rate-dependent bandwidth pressure

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
- coalescing or rebuild strategy if partial residency is used
- explicit entry-count limit
- diagnostics for fragmentation and largest free span

## Measurements

Record at minimum:
- total/free internal SRAM
- largest free internal block
- total/free PSRAM
- largest free PSRAM block
- sample arena total/used/free
- resident sample count
- allocation failures
- fragmentation/repack events

Memory design changes should include before/after measurements on hardware.
