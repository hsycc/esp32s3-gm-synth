# Architecture

## System goals

The system is a realtime GM1 sample synthesizer for ESP32-S3. It is optimized for a known/validated GM SoundFont and measurable embedded realtime behavior.

## High-level data flow

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

Asset/control side:

```text
SF2 / compiled bank
        │
        ▼
 Parser / Bank Reader
        │
        ▼
Sample Residency Manager
   ┌──────────────┴──────────────┐
   ▼                             ▼
PSRAM Resident              Future StreamSource
                                  │
                                  ▼
                                  SD
```

## Runtime domains

### Audio realtime domain

Responsibilities:
- consume bounded timestamped events
- execute events at frame offsets inside the current block
- allocate/steal voices without blocking
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
- decode/control non-audio state
- parse/load assets
- prepare Program Change state
- maintain sample references/residency
- update slow control-rate coefficients
- aggregate diagnostics and print/log outside the hot path

### Host tooling domain

Responsibilities:
- analyze the target SoundFont
- generate machine-readable reports
- optionally compile SF2 into a future custom bank/index representation
- generate deterministic test vectors/MIDI fixtures

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

Owns SF2 structure parsing and zone resolution. Parsing and sample I/O should be separable. The parser should not be the long-term owner of runtime PCM.

### `sample/`

Owns sample handles, residency, references and storage-source abstraction.

Initial implementation prefers PSRAM-resident PCM.

Long-term interface should allow:
- resident source
- streamed source

without requiring the core voice DSP to know the storage backend.

### `synth/`

Owns:
- voice pool
- allocation
- score/steal policy
- per-voice runtime state
- note lifecycle

A voice corresponds to one active sample/zone playback instance. A single MIDI note can create multiple voices.

### `dsp/`

Owns deterministic audio primitives:
- interpolation
- envelope
- filter
- LFO/control-rate helpers
- chorus/reverb/limiter

Move expensive invariant calculations to control/block rate where possible.

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

Realtime code only increments/records bounded counters; formatting/logging happens elsewhere.

## Program Change model

Avoid synchronous “pause -> load -> resume” in the final design.

Preferred path:

```text
request program
     │
     ▼
resolve required samples/zones
     │
     ▼
prepare outside audio task
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

If M1 proves the entire fixed GM PCM set fits PSRAM with adequate reserve, prefer full startup residency and make Program Change an index/state transition.

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
- storage/sample pressure if streaming is later added

The target policy is:
- guarantee 16 voices after validation
- allow 18–20 when measured headroom permits
- reduce admission gradually under pressure
- recover gradually after sustained stability

## Design principle

The project should behave like a small realtime audio engine, not like a desktop sampler port squeezed onto an MCU.
