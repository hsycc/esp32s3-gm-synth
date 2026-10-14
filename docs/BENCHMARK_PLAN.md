# Benchmark Plan

## Objective

Create repeatable measurements that answer whether the ESP32-S3 target is safe at the intended audio quality and polyphony.

Do not treat compile-time voice limits as evidence of stable realtime capacity.

## Hardware baseline record

Every benchmark result must record:
- exact ESP32-S3 board/module
- CPU frequency
- flash size/mode
- PSRAM size/mode/frequency
- ESP-IDF / Arduino core version
- compiler flags
- DAC / codec and I2S configuration
- sample rate / sample format
- audio block size
- enabled DSP/FX features
- SoundFont/bank revision or hash

## Core realtime metrics

Per audio block collect bounded telemetry for:
- render duration in microseconds or CPU cycles
- peak render duration
- deadline margin
- overrun/deadline miss count
- I2S underrun count if observable
- active voice count
- voice admission limit
- event queue depth/high-water mark
- event queue drop count
- lock/contention skip count if any
- invalid sample/fetch count

Outside the realtime path aggregate:
- min / mean / max
- p95 / p99 / p99.9 render time when sufficient samples exist
- distribution by voice count

## Block deadline

At 44.1 kHz and 64 frames:

`64 / 44100 ~= 1.451 ms`

This is the block-time scale. The renderer should retain meaningful headroom rather than merely staying below the deadline on average.

## Voice-load matrix

Measure at minimum:
- 1 voice
- 4 voices
- 8 voices
- 12 voices
- 16 voices
- 18 voices
- 20 voices if reachable

Repeat with:
- dry only
- reverb enabled
- chorus enabled
- filter paths enabled as applicable
- worst known layered preset

## Musical stress cases

### Dense piano
- repeated 8–10 note chords
- sustain held
- retrigger while releases remain active

### Same-note retrigger
- rapid repeated Note On of same pitch
- validates duplicate-note policy and steal behavior

### Drum burst
- dense percussion notes with overlapping tails

### Multi-channel GM
- 16-channel MIDI mix
- drums + bass + chord + melody + sustaining pads

### Program Change
- repeated changes between small and large working-set presets
- record audible gap / preparation time / commit time

### Pitch stress
- pitch bend and transposition cases that increase source-read rate or DSP cost

## Memory metrics

At boot and after worst-case asset loads record:
- free internal SRAM
- largest internal SRAM block
- free PSRAM
- largest PSRAM block
- sample-pool capacity
- sample-pool used bytes
- sample entry count
- fragmentation indicators if applicable

## Pass/fail philosophy

A target voice count is only considered stable when the relevant stress suite completes with:
- zero audio deadline misses
- zero invalid sample accesses
- zero queue drops
- zero unrecovered underruns
- acceptable audible behavior during voice stealing
- repeatable results over long runs

The guaranteed voice count should include margin below the highest merely-achievable count.
