# Requirements

## Product scope

V1 is a software GM/SoundFont sound module for **ESP32-S3 N16R8** with **native user-loadable SF2 support**.

Users should be able to create and share `.sf2` banks using common SoundFont tools and load compatible banks without converting them to a proprietary format.

A bundled GM bank is the default/qualification asset, not the only supported bank.

### Required musical behavior

- 16 MIDI channels
- GM1 melodic programs 0–127 for the bundled qualification bank
- channel-10 drum/percussion behavior
- Note On / Note Off
- Program Change
- Bank Select
- pitch bend
- configurable pitch-bend range through the supported RPN path
- sustain pedal (CC64)
- channel volume (CC7)
- pan (CC10)
- expression (CC11)
- reverb send (CC91)
- chorus send (CC93)
- GM reset behavior required by the validation corpus

Broader GS/XG behavior is optional, but non-zero banks in user SF2 files should be parsed and exposed where practical.

## Audio targets

Initial baseline:
- 44,100 Hz
- stereo output
- 16-bit I2S output format
- 64-frame render blocks
- linear interpolation initially unless measurements justify another method
- global send reverb and chorus

### Polyphony

- 16 active sample voices: qualification target / intended guaranteed minimum
- 18–20 voices: stretch target only when measured realtime margin supports it

A voice means one active sample/zone playback instance, not one MIDI note. Stereo-linked samples and layered zones must be accounted for separately in performance validation.

## Fixed hardware baseline

- ESP32-S3 @ 240 MHz
- **N16R8 only**
- 16 MB flash
- 8 MB PSRAM
- I2S DAC or equivalent external audio codec/output stage

R16 must not be assumed for capacity planning.

Specific board/pin assignments are intentionally deferred until the board baseline is selected, but memory-capacity decisions are bound to N16R8.

## SF2 user-asset requirements

`.sf2` is the primary exchange/import format for V1.

The product must distinguish three independent limits:

1. **Storage limit** — whether the SF2 file fits the available flash/filesystem/external storage.
2. **Metadata/feature compatibility** — whether the firmware understands the SF2 generators/modulators/sample structures used by the bank.
3. **Active residency limit** — whether the currently selected programs' PCM working set fits R8 PSRAM with safe system reserve.

Do not reject or accept a SoundFont based only on total `.sf2` file size.

A user SF2 import/load operation should produce a compatibility report or status containing at least:
- SF2 version/basic RIFF validity
- preset/instrument/sample counts
- storage/file size
- generator usage
- modulator usage
- stereo-link usage
- sample-rate distribution
- estimated per-program and current multi-channel residency
- unsupported features, if any

## Compatibility target classes

### Open/simple baseline

`FM/GM Open SoundFont` is useful as a simple public GM128 baseline:
- Bank 0 programs 0–127
- GM standard drum kit
- 44.1 kHz / 16-bit / mono
- deliberately small/simple generator set

Passing this bank proves parser/GM-map basics, not full SoundFont compatibility.

### Real-world compatibility baseline

`GeneralUser GS` is used as a high-value real-world compatibility reference because its author explicitly documents extensive use of SoundFont synthesis and modulator features and dependence on standards-compliant players.

### Compact real-world baseline

`TimGM6mb.sf2` remains useful for N16R8 memory/performance testing and modulator behavior.

### Non-standard stress input

`GMBK5X128.sf2` is useful for large-bank residency/storage stress, but must not define standard SF2 behavior because the supplied file is known to be non-standard.

## SF2 feature roadmap

V1 compatibility work should prioritize:
- correct preset/instrument global + local zone hierarchy
- key/velocity range intersection
- sample addressing and loop offsets
- root key / coarse tune / fine tune / scale tuning
- volume envelope
- modulation envelope features required by reference banks
- initial attenuation
- pan
- filter cutoff/Q
- reverb/chorus sends
- sample modes and exclusive class
- standard/default SF2 modulators needed for normal MIDI behavior
- `imod` / `pmod` semantics required by GeneralUser/Tim-style banks
- stereo-linked samples

Features outside the supported profile must be detected and reported rather than silently treated as fully compatible.

SF3 is not a V1 requirement. It may be added later as an optional compressed SoundFont input format.

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

Reason: user-loadable SF2 banks can be much larger than R8 even when their currently selected 16-channel sample union fits in R8.

Preferred policy:
1. full-bank residency is an automatic fast path for small SF2 banks that fit with measured reserve
2. shared/per-program PSRAM residency is the general path
3. future storage streaming is optional and should not be required for ordinary SF2 operation

No storage I/O is allowed in Note On execution in V1.

## SF2 indexing/cache

A proprietary bank conversion is **not** required before playback.

The implementation may generate a small derived sidecar/index cache from the source SF2 to accelerate subsequent loads. Such a cache may contain:
- preset/bank lookup tables
- program -> sample dependency sets
- validated sample file offsets and lengths
- normalized/resolved zone metadata where semantics are preserved

The cache must be disposable/regenerable; the `.sf2` remains the shareable source of truth.

## Program Change

Final Program Change must not load samples inside the audio task.

Acceptable models:
- small/full-resident SF2: pointer/index/state switch
- partial residency: asynchronous/control-domain prepare, then safe commit at an audio block boundary

Program transition latency and any audible gap must be measured.

If the requested program cannot fit together with currently pinned active program samples, the system must fail gracefully or apply a documented policy; it must not corrupt active voice memory.

## Storage planning

N16 flash does not imply that a 16 MB SF2 file can be stored internally. Firmware image(s), OTA slots, NVS/filesystem metadata and other assets also consume flash.

The internal user-SF2 size limit must be derived from the final partition table and measured firmware size. Larger SF2 support may require external storage in a later product configuration.

## Quality gates

GM128 qualification is incomplete until tests cover:
- every melodic program in the default GM bank
- standard drum paths
- representative key and velocity ranges
- sustain-heavy playback
- repeated retriggers
- rapid Program Change
- 16-channel mixed playback
- layered zones
- stereo-linked samples
- default and custom modulators
- voice stealing
- long-running stress

Release reports must contain hardware measurements on N16R8, not only host simulations.
