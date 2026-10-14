# Requirements

## Product scope

V1 is a software GM1 sound module for ESP32-S3 using a fixed, validated SoundFont.

### Required musical behavior

- 16 MIDI channels
- GM1 melodic programs 0–127
- channel-10 drum/percussion behavior
- Note On / Note Off
- Program Change
- Bank Select sufficient for the validated bank
- pitch bend
- configurable pitch-bend range through the supported RPN path
- sustain pedal (CC64)
- channel volume (CC7)
- pan (CC10)
- expression (CC11)
- reverb send (CC91)
- chorus send (CC93)
- GM reset behavior required by the validation corpus

Broader GS/XG support is optional and must not delay GM1 qualification.

## Audio targets

Initial baseline:
- 44,100 Hz
- stereo output
- 16-bit I2S output format
- 64-frame render blocks
- linear interpolation initially unless measurements justify another method
- global send reverb and chorus

### Polyphony

- 16 voices: qualification target / intended guaranteed minimum
- 18–20 voices: stretch target only when measured realtime margin supports it

A voice means one active sample/zone playback instance, not one MIDI note.

## Hardware assumptions

Initial target class:
- ESP32-S3 @ 240 MHz
- PSRAM required
- 16 MB PSRAM preferred for initial full-residency investigation
- I2S DAC or equivalent external audio codec/output stage

Specific board/pin assignments are intentionally deferred until the hardware baseline is selected.

## SoundFont scope

V1 optimizes for one validated GM bank rather than arbitrary SF2 compatibility.

Before implementation decisions rely on the bank, M1 must determine:
- actual `smpl` payload size
- sample count
- preset/instrument count
- generator usage
- modulator usage
- stereo-link usage
- maximum relevant zone layering
- per-program sample working set
- union working sets for realistic 16-channel mixes

## Realtime requirements

The audio realtime path must be bounded.

It must not:
- perform filesystem/SD/flash asset reads
- allocate/free general heap memory
- parse SF2
- wait on blocking locks
- format/print logs
- write persistent storage

It must provide telemetry for:
- render duration
- peak render duration
- deadline miss/overrun count
- underrun count
- queue overflow/drop count
- invalid sample access/fetch count
- voice stealing count
- current and configured voice limits

## Event timing

The final event path must support timestamped events and intra-block execution offsets. Applying all events only at block boundaries is acceptable for an early bootstrap, but not the intended final architecture.

## Sample residency

Preference order:
1. full fixed-bank residency in PSRAM, if M1 proves it fits with adequate system reserve
2. shared/per-program PSRAM residency
3. streaming only for future banks whose size/quality justifies the additional storage-latency complexity

No storage I/O is allowed in Note On execution in V1.

## Program Change

Final Program Change must not depend on loading assets in the audio task.

Acceptable models:
- full-resident bank: pointer/index/state switch
- partial residency: asynchronous/control-domain prepare, then safe commit at a block boundary

Program transition latency and any audible gap must be measured.

## Quality gates

GM128 qualification is incomplete until tests cover:
- every melodic program
- drum kit paths used by the bank
- representative key and velocity ranges
- sustain-heavy playback
- repeated retriggers
- rapid Program Change
- 16-channel mixed playback
- voice stealing
- long-running stress

Release reports must contain hardware measurements, not only host simulations.
