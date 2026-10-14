# Requirements

## Product scope

V1 is a software GM1 sound module for **ESP32-S3 N16R8** using a validated SoundFont-derived asset profile.

### Required musical behavior

- 16 MIDI channels
- GM1 melodic programs 0–127
- channel-10 drum/percussion behavior
- Note On / Note Off
- Program Change
- Bank Select sufficient for the validated bank/profile
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

## Fixed hardware baseline

- ESP32-S3 @ 240 MHz
- **N16R8 only**
- 16 MB flash
- 8 MB PSRAM
- I2S DAC or equivalent external audio codec/output stage

R16 must not be assumed for capacity planning.

Specific board/pin assignments are intentionally deferred until the board baseline is selected, but memory-capacity decisions are already bound to N16R8.

## SoundFont scope

V1 does not require support for a fixed `.sf2` file size such as 14 MB.

The relevant limits are:
- source asset footprint in N16 flash
- compiled GM-only asset footprint
- active/shared PCM working set in R8 PSRAM
- SF2 generator/modulator features actually used
- runtime CPU cost of the selected bank/profile

Current real candidate banks are documented in `SF2_BANK_ANALYSIS.md`.

Before a bank is qualified, M1/M2 must determine:
- actual `smpl` payload size
- sample count
- preset/instrument count
- bank/preset distribution
- generator usage
- modulator usage
- stereo-link usage
- source sample-rate distribution
- maximum relevant zone layering
- per-program sample working set
- realistic 16-channel union working sets
- GM-only pruning opportunity

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
- sample residency usage / allocation failures

## Event timing

The final event path must support timestamped events and intra-block execution offsets. Applying all events only at block boundaries is acceptable for an early bootstrap, but not the intended final architecture.

## Sample residency

The architecture **must support shared/per-program PSRAM residency**.

Reason: at least one real candidate source bank exceeds R8 by a large margin, while its realistic 16-channel active union can still fit within R8.

Preference order for a specific compiled bank:
1. full fixed-bank residency only if that bank fits in R8 with adequate measured system reserve
2. shared/per-program PSRAM residency as the general V1 path
3. streaming only when future requirements cannot be met by compiled/pruned assets plus residency management

No storage I/O is allowed in Note On execution in V1.

## Asset compilation / pruning

A host-side compilation stage is permitted and expected.

It may:
- remove non-GM banks/presets that the product does not expose
- deduplicate referenced samples
- flatten/normalize metadata needed by firmware
- precompute program -> sample dependency sets
- precompute resolved zone metadata where safe
- optionally resample source PCM when justified by measured quality/storage/CPU tradeoffs

The compiler must preserve intended playback semantics for the qualified bank/profile.

## Program Change

Final Program Change must not depend on loading assets in the audio task.

Acceptable models:
- full-resident compiled bank: pointer/index/state switch
- partial residency: asynchronous/control-domain prepare, then safe commit at a block boundary

Program transition latency and any audible gap must be measured.

## Quality gates

GM128 qualification is incomplete until tests cover:
- every melodic program
- drum kit paths used by the selected profile
- representative key and velocity ranges
- sustain-heavy playback
- repeated retriggers
- rapid Program Change
- 16-channel mixed playback
- voice stealing
- long-running stress

Release reports must contain hardware measurements on N16R8, not only host simulations.
