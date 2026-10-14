# Candidate SoundFont Analysis — N16R8

This document records measured characteristics of two user-supplied candidate SoundFonts for the fixed **ESP32-S3 N16R8** target.

The source `.sf2` files are intentionally not committed. SHA-256 is recorded so later reports can be tied to the exact binaries.

## Source identity

| File | SHA-256 |
|---|---|
| `TimGM6mb.sf2` | `82475b91a76de15cb28a104707d3247ba932e228bada3f47bba63c6b31aaf7a1` |
| `GMBK5X128.sf2` | `3bddbc79452f5e261eca1ecc6ed9f4c729429fa1d2f07f0aeab94ca3429fa1db` |

## High-level comparison

| Metric | TimGM6mb | GMBK5X128 |
|---|---:|---:|
| Source file | 5.72 MiB | 15.06 MiB |
| `smpl` payload | 5.52 MiB | 14.92 MiB |
| Aligned sample PCM sum | 5.47 MiB | 14.84 MiB |
| Presets | 136 | 407 |
| Instruments | 210 | 281 |
| Samples | 520 | 887 |
| GM bank-0 presets | 128 | 128 |
| Bank-128 drum presets | 8 | 10 |
| Stereo-linked samples | 0 | 0 |
| Sample channels represented | mono only | mono only |
| Source modulators | substantial `imod` use | none observed |

## Sample-rate profile

### TimGM6mb

Main sample-rate counts:
- 44,100 Hz: 334 samples
- 22,050 Hz: 92
- 22,500 Hz: 47
- 12,000 Hz: 40
- 11,025 Hz: 4
- a few uncommon rates also occur

This bank is already storage-conscious and mixes multiple source rates.

### GMBK5X128

All 887 sample headers report **48,000 Hz**.

With a 44.1 kHz output engine, source playback therefore always carries a 48k -> 44.1k base phase ratio before note pitch/tuning is applied. Offline 48k -> 44.1k conversion is a possible product-asset optimization, but must be evaluated for sound quality and does not remove the need for pitch interpolation.

## Bank distribution

### TimGM6mb

- bank 0: 128 presets
- bank 128: 8 drum presets

It is close to a compact GM-focused source bank.

### GMBK5X128

The source contains 407 presets over many banks. Major counts include:
- bank 0: 128
- bank 127: 128
- bank 8: 35
- bank 1: 30
- bank 2: 15
- bank 9: 11
- bank 16: 11
- bank 128: 10
- additional smaller variation banks

For a GM1 product, most of this source content is not required. The source SF2 should therefore be treated as an **authoring/input asset**, not automatically as the deployable flash asset.

## R8 residency analysis

R8 provides 8 MiB PSRAM total, not 8 MiB usable PCM capacity. Firmware, effects, metadata, allocator overhead and safety reserve all consume part of it.

### TimGM6mb

Complete aligned PCM: **~5.47 MiB**.

Gross PSRAM left if every sample were resident:

`8.00 - 5.47 = ~2.53 MiB`

This makes full residency plausible, but not guaranteed. It must be validated after real firmware/FX/task memory is measured on N16R8.

Bank-0 + standard drum union is approximately **5.41 MiB**.

### GMBK5X128

Complete aligned PCM: **~14.84 MiB** — impossible on R8.

Bank-0 sample union alone: **~7.24 MiB**.

Bank-0 + standard drum union: **~8.49 MiB** — still impossible as a safe full-resident configuration on R8.

Therefore this bank requires a residency manager even after GM-only pruning.

## 16-channel working-set analysis

The relevant runtime question is not total bank size, but the union of samples required by the active 16 MIDI channel programs.

### TimGM6mb

- max 16 melodic bank-0 union found: **~3.67 MiB**
- 15 melodic + standard drum: **~4.00 MiB**
- worst 15 melodic + drum across its drum presets: **~4.02 MiB**

### GMBK5X128

- max 16 melodic bank-0 union found: **~4.30 MiB**
- 15 melodic + standard drum: **~5.44 MiB**
- worst 15 melodic + drum across its drum presets: **~5.80 MiB**

For these files, the analyzer's greedy maximum-union selections were independently checked during analysis against an exact binary optimization calculation and produced the same union sizes. The repository tool remains standard-library-only and continues to label its generic greedy result as a heuristic for arbitrary future banks.

These figures strongly support **shared/per-program residency** on R8 for the larger bank.

## Per-program/layer observations

### TimGM6mb

- average bank-0 resolved zones: ~10.1
- median bank-0 resolved zones: 8
- maximum bank-0 resolved zones: 68
- maximum observed simultaneous zone overlap at one key/velocity point: **6**
- largest single preset PCM working set observed: ~0.67 MiB (`Strings CLP`)

### GMBK5X128

- average bank-0 resolved zones: ~6.6
- median bank-0 resolved zones: 6
- maximum bank-0 resolved zones: 18
- maximum observed simultaneous zone overlap at one key/velocity point: **3**
- largest source preset working sets are drum kits; largest observed is ~1.62 MiB

Remember: one MIDI note may consume several sample voices when zones overlap. A nominal 16-voice engine therefore does not guarantee 16 simultaneously held notes for every preset.

## Modulators and SF2 semantic complexity

### TimGM6mb

No non-terminal preset modulators were found, but **455 instrument-modulator records** are present.

- 193 have non-zero amount
- 262 have amount 0; zero-amount records may still be semantically relevant because SF2 modulators can override/disable defaults

Non-zero-amount destination usage includes:
- reverb send: 64
- pan: 57
- chorus send: 57
- mod-LFO-to-pitch: 10
- vibrato-LFO-to-pitch: 5

Other zero-amount records frequently target filter cutoff and vibrato behavior.

Therefore a runtime parser that simply skips `imod` cannot be considered faithful for this bank without listening/reference validation or offline normalization.

### GMBK5X128

No effective `pmod` or `imod` records were observed in the analyzed source.

This is a major implementation advantage: despite being much larger in sample data, its SF2 control semantics are materially simpler.

## Generator complexity

`GMBK5X128` uses a relatively small generator subset, dominated by:
- key range
- velocity range
- sample ID
- sample modes
- overriding root key
- fine tune
- volume envelope decay/sustain/release
- initial attenuation
- initial filter cutoff
- pan
- exclusive class

`TimGM6mb` uses a much broader set including mod/vibrato LFO, modulation envelopes, filter Q, scale tuning, loop offsets, key-dependent envelope decay and other controls.

This means **small file size does not imply simpler synthesizer compatibility**.

## Important reference-engine gap

The current copych reference engine has a `zone.attenuation` field that is consumed by voice gain, but source inspection did not find a corresponding `InitialAttenuation` case in its `applyGenerators()` implementation. Both real candidate banks use SF2 `initialAttenuation` extensively.

Therefore our firmware must explicitly audit and implement generator semantics rather than assuming the reference project's current support is sufficient.

## N16 flash implications

### TimGM6mb

The ~5.72 MiB source easily leaves room in 16 MB flash for firmware and product partitions.

### GMBK5X128

The raw ~15.06 MiB source leaves less than 1 MiB of gross capacity in a 16 MiB flash before firmware, partition table, NVS and other assets. Shipping it raw is therefore not a viable default product layout.

However its source contains many non-GM banks. A host compiler that retains only GM bank 0 plus the selected drum kit can reduce persistent sample requirements substantially. For example, bank 0 + standard drum references about **8.49 MiB** of the original 48 kHz PCM before any optional resampling/format compaction.

## Architecture consequence

V1 should implement one common runtime architecture:

```text
Source SF2
   ↓
Host analyzer/compiler
   ↓
Compiled GM asset bank in N16 flash
   ↓
Shared Sample Residency Manager
   ↓
R8 PSRAM SamplePool
   ↓
Voice Engine
```

With two optimization modes:

1. **Full-resident fast path** when a compact compiled bank demonstrably fits R8 with reserve.
2. **Shared/per-program residency** as the mandatory general path.

Do not design two different synthesizers for the two source banks.

## Current recommendation

Keep both banks in the qualification program:

- `TimGM6mb` is the compact-memory / complex-SF2-semantics test profile.
- `GMBK5X128` is the large-asset / residency-manager / simpler-SF2-semantics test profile.

Sound quality should ultimately decide which bank (or derived bank) becomes the product default. Architecture should support both profiles on N16R8 without relying on R16.
