# SF2 Compatibility Baselines

## Why this document exists

The product goal is native user-loadable `.sf2` support on ESP32-S3 N16R8. One SoundFont must not become the de-facto specification.

Use multiple public/reference banks to exercise different parts of SoundFont behavior.

## Baseline set

### 1. FM/GM Open SoundFont — simple/open baseline

Public project: `zeittresor/opensoundfont`.

Published properties:
- SoundFont 2 / `.sf2`
- 44.1 kHz / 16-bit / mono
- GM melodic programs 0–127
- GM standard drum kit
- GPL-3.0-only
- compact edition about 14.34 MB
- melodic presets intentionally use a simple single-root-sample strategy
- documented generator set is intentionally small

Use it for:
- parser correctness
- bank/program mapping
- ordinary generator handling
- drum mapping
- large-file / partial residency tests

Do **not** use it alone to claim broad SF2 compatibility because its synthesis structure is intentionally simple.

### 2. GeneralUser GS — real-world standards/compliance baseline

Current public documentation (v2.0.3 in 2026) describes:
- 261 instrument presets
- 13 drum kits
- about 30.7 MB RAM footprint
- heavy use of SoundFont synthesis and modulator features
- a strong dependence on a standards-compliant SF2 player

The author specifically documents poor rendering on TinySoundFont because of important specification gaps including lack of modulator support.

Use GeneralUser GS to validate:
- standard/default SF2 modulators
- `imod` / `pmod`
- generator merge semantics
- attenuation/filter/controller behavior
- real-world compatibility beyond simple GM playback
- partial sample residency

GeneralUser GS is a real-world compatibility reference; its custom redistribution license is not the same thing as an OSI software license.

### 3. FluidR3_GM — large classic open GM baseline

FluidR3_GM is a long-standing GM SoundFont distributed under the MIT license and commonly used with FluidSynth/Linux audio stacks.

The classic stereo GM bank is roughly 140–150 MB, far larger than N16 internal flash or R8 PSRAM.

Use it as an architectural stress case proving that:
- SF2 file size and active working set are different limits
- whole-bank residency cannot be the general architecture
- internal flash cannot be the only storage answer for arbitrary large user banks

V1 does not require an N16-only device to store FluidR3 internally; this bank is a compatibility/storage architecture reference.

### 4. TimGM6mb — compact real-world N16R8 baseline

The analyzed user-supplied copy is approximately 5.7 MB and fits the compact-device use case well.

Use it for:
- possible full-resident fast path
- mixed source sample-rate behavior
- modulator coverage
- dense zone/layer tests

### 5. GMBK5X128 — non-standard stress asset

The supplied file is known to be non-standard.

Use it only for:
- parser robustness
- large-bank storage/residency stress
- failure/warning behavior

Do not derive SF2 semantic requirements from this file.

## Compatibility policy

### V1 target

Support a documented SoundFont 2.x profile broad enough for ordinary user-authored banks and important real-world GM banks.

Priority features:
- RIFF `sfbk` / `INFO` / `sdta` / `pdta`
- `phdr/pbag/pmod/pgen/inst/ibag/imod/igen/shdr`
- preset and instrument global/local zone hierarchy
- key/velocity ranges
- sampleID/instrument routing
- sample loop/address offsets
- root key and tuning
- scale tuning
- volume envelope
- modulation envelope where used by qualification banks
- initial attenuation
- filter cutoff/Q
- pan
- reverb/chorus sends
- sample modes
- exclusive class
- standard default modulators
- custom instrument/preset modulators
- stereo-linked samples

### Explicit non-goals for early V1

Until profiled/implemented:
- claim of complete arbitrary SF2 compatibility
- SF2 24-bit `sm24` playback
- ROM sample types
- SF3/Vorbis decoding
- rare features not exercised by qualification banks

Unsupported features must be reported during import/load.

## Product-facing behavior

The shared asset is always the original `.sf2` file.

Firmware may create a disposable sidecar cache/index containing:
- validated table offsets
- preset/bank lookup
- program -> sample dependency sets
- sample storage offsets and lengths
- normalized zone metadata

This cache is an implementation detail and must be regenerable from the SF2.

## Memory model implication

N16R8 must support two residency modes:

1. **Full resident** — automatic optimization for small banks that fit safely.
2. **Shared/per-program resident** — general case for larger user SoundFonts.

Program selection is a prepare/commit transaction. Storage reads and sample allocation happen outside the audio realtime task.

## Reference-render comparison

Where practical, host tests should compare selected notes/controllers against FluidSynth because FluidSynth explicitly aims for close SoundFont-spec compliance.

Comparisons should cover:
- pitch
- amplitude/attenuation
- envelope timing
- pan
- filter behavior
- mod wheel / expression / volume
- pitch bend
- reverb/chorus sends
- custom modulators

The goal is not bit-identical output; the goal is detecting semantic mismatches in SF2 interpretation.
