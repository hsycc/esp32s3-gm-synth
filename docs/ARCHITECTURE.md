# Architecture

## System goals

The system is a realtime GM/SoundFont sample synthesizer for **ESP32-S3 N16R8**.

The product-facing asset format is **standard `.sf2`** so users can create, import and share SoundFonts with common ecosystem tools. The firmware may build disposable derived indexes/caches, but it must not require users to convert SF2 into a proprietary format before playback.

## High-level data flow

```text
User / Bundled .sf2
        │
        ▼
  SF2 Validator / Parser
        │
        ├── Preset / Instrument / Zone metadata
        ├── Generator / Modulator metadata
        ├── Sample offsets / loop metadata
        └── Optional disposable sidecar index
        │
        ▼
Sample Residency Manager
        │
        ├── full-resident fast path (small bank)
        └── shared/per-program residency (general path)
        │
        ▼
      R8 PSRAM
        │
        ▼
     Voice Engine
```

Musical realtime path:

```text
USB MIDI / UART MIDI / Internal Events
                  │
                  ▼
             MIDI Decoder
                  │
              timestamp
                  │
                  ▼
             Event Queue
                  │
                  ▼
         Audio Block Scheduler
                  │
                  ▼
        GM Channel State (16ch)
                  │
                  ▼
             Zone Resolver
                  │
                  ▼
        Adaptive Voice Manager
                  │
          ┌───────┴────────┐
          ▼                ▼
       Voice 0          Voice N
          │                │
          └───────┬────────┘
                  ▼
                Mixer
          ┌───────┴────────┐
          ▼                ▼
       Chorus            Reverb
          └───────┬────────┘
                  ▼
            DC / Limiter
                  │
                  ▼
                 I2S
```

## Runtime domains

### Audio realtime domain

Responsibilities:
- consume bounded timestamped events
- execute events at frame offsets inside the current block
- allocate/steal voices without blocking
- apply already-prepared generator/modulator state
- run voice DSP
- mix dry/FX sends
- run block/global FX
- write or hand off audio buffers to I2S
- collect lightweight realtime counters

Forbidden work:
- filesystem I/O
- SF2 parsing
- heap allocation/free
- blocking synchronization
- logging
- flash writes

### Control / asset domain

Responsibilities:
- validate/parse user SF2 assets
- maintain parsed/indexed SoundFont metadata
- resolve preset/instrument/zone hierarchy
- evaluate slow/control-rate SF2 modulator changes where appropriate
- prepare Program Change state
- load missing samples from SF2 storage into PSRAM
- maintain sample references/residency
- update slow control-rate coefficients
- aggregate diagnostics/log outside the hot path

### Host tooling domain

Responsibilities:
- analyze public/reference SoundFonts
- generate compatibility reports
- generate deterministic SF2/MIDI test fixtures
- compare outputs against a reference synth where practical
- optionally generate sidecar/index fixtures for tests

A host-side proprietary bank compiler is no longer a product requirement.

## Core modules

### `midi/`

Owns byte/protocol parsing and converts input into compact internal events. It does not directly perform heavy synth work in transport callbacks.

### `gm/`

Owns per-channel GM/MIDI semantic state:
- bank/program
- volume/expression/pan
- sustain
- pitch bend/range
- RPN state
- drums/channel mode where applicable

Do not place sample-cache ownership in generic MIDI state objects.

### `sf2/`

Owns SF2 parsing and semantics:
- RIFF/LIST structure
- `pdta` / `sdta`
- preset / instrument / sample tables
- global/local zone hierarchy
- generator merge/application rules
- default modulators
- `pmod` / `imod`
- stereo/sample-link metadata
- feature/compatibility reporting

Parsing and sample I/O must be separable. The parser owns metadata, not runtime PCM lifetime.

### `sample/`

Owns:
- stable sample handles
- PSRAM arena/pool
- sample residency and references
- SF2 file offsets/lengths
- program -> sample dependency sets
- safe prepare/retire behavior

General path is partial/shared residency. Full residency is an optimization selected automatically when a bank fits.

### `synth/`

Owns:
- voice pool
- allocation
- score/steal policy
- per-voice runtime state
- note lifecycle
- mapped SF2 generator/modulator state

A single MIDI note may create multiple active sample render lanes because of layering or stereo-linked samples.

### `dsp/`

Owns deterministic audio primitives:
- interpolation
- envelope
- filter
- LFO/control-rate helpers
- chorus/reverb/limiter

Move expensive invariant calculations to note/control/block rate where possible.

### `audio/`

Owns block rendering, frame-offset event execution, buffer lifecycle and I2S integration.

### `diagnostics/`

Owns:
- render duration
- deadline margin
- overruns/underruns
- queue drops
- voice steal counters
- invalid sample access counters
- memory high-water marks
- sample residency pressure
- SF2 compatibility/import warnings

Realtime code only increments/records bounded counters; formatting/logging happens elsewhere.

## SF2 import/index lifecycle

Preferred flow:

```text
receive / select .sf2
      │
      ▼
validate RIFF/SF2 structure
      │
      ▼
parse metadata + feature scan
      │
      ├── compatible -> register bank
      ├── partially supported -> warn/report
      └── unsupported/invalid -> reject safely
      │
      ▼
optional small sidecar index/cache
      │
      ▼
SF2 remains source of truth
```

A sidecar cache may store offsets and precomputed dependency/index data, but not require repackaging the original asset. It must be disposable and regenerable.

## Sample residency model

N16R8 cannot assume whole-bank residency for user SoundFonts.

```text
selected programs on 16 channels
            │
            ▼
      required sample union
            │
      ┌─────┴─────┐
      │ already   │ missing
      │ resident  ▼
      │        async load from SF2
      │            │
      └──────┬─────┘
             ▼
         PSRAM pool
```

Samples referenced by active voices remain pinned even when a channel changes programs until those voices are safely retired.

If a requested program set cannot fit, preparation fails before commit and the existing realtime state remains valid.

## Program Change model

Avoid synchronous `pause -> load -> resume` in the final design.

Preferred path:

```text
request program
     │
     ▼
resolve required samples/zones
     │
     ▼
prepare missing PCM outside audio task
     │
     ▼
READY
     │
     ▼
commit at safe block boundary
     │
     ▼
retire old state safely
```

For a small SF2 that fully fits R8, Program Change naturally reduces to an index/state transition.

## Modulator execution model

Supporting user-created SF2 requires more than reading `pmod/imod` records.

Modulator work should be separated by update frequency:
- note-static sources (key, velocity): evaluate on voice start
- channel/controller sources (CC, pitch bend, pressure): update when source changes
- LFO/envelope destinations: keep only the minimum per-sample work in the hot loop

Do not blindly recompute a generic modulator graph for every voice on every sample.

## Compatibility tiers

### Tier A — basic/GM SF2

Covers simple public GM banks with ordinary generators, mono samples and standard drum maps.

### Tier B — real-world SoundFont 2.x

Adds correct default/custom modulators, generator hierarchy, stereo-linked samples and behavior required by banks such as GeneralUser GS.

### Tier C — optional/advanced features

Rare/expensive SoundFont features, 24-bit extensions or future SF3 support may be added only after N16R8 profiling.

The UI/import report should make compatibility level visible instead of pretending all SF2 files are equivalent.

## Realtime budget

At 44.1 kHz with 64-frame blocks:

- one block duration is ~1.451 ms
- this is the render deadline scale, not total end-to-end latency

The design should measure actual render times on hardware and preserve margin for interrupts, I2S service and control variability.

## Polyphony policy

Compile-time maximum is only a hard cap.

Runtime admission limit may be lower based on:
- recent render pressure
- deadline misses
- zone layering
- stereo sample cost
- current sample-residency pressure

The target policy is:
- guarantee 16 sample render voices after validation
- allow 18–20 when measured headroom permits
- reduce admission gradually under pressure
- recover gradually after sustained stability

## Design principle

The project should be a small standards-aware realtime SoundFont engine designed around N16R8 constraints, not a desktop sampler squeezed onto an MCU and not a closed proprietary-bank player.
