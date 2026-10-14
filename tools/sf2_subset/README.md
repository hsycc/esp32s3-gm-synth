# SF2 Subset Builder

Host-side tool for generating deterministic, valid SoundFont 2 subsets for cross-bank compatibility tests.

The goal is not to create a product format. The user-facing asset remains `.sf2`.

## Why subsets exist

The public compatibility banks have very different source sizes:

- FM/GM Open SoundFont: ~14 MiB compact build
- GeneralUser GS: ~31 MiB
- FluidR3_GM: ~148 MiB

For repeatable N16R8 parser/resolver tests we also keep derived subsets near 6 MiB. This gives CI/hardware tests similarly sized assets without pretending that file size is the runtime PSRAM working set.

## Selection policy

The default selector:

1. Includes bank 128 / program 0 (Standard Drum) when present.
2. Walks the 16 General MIDI instrument families round-robin:
   - 0, 8, 16, ... 120
   - then 1, 9, 17, ... 121
   - and so on.
3. Adds a program only while the estimated rebuilt SF2 stays within the target budget.
4. Preserves the selected presets' referenced instruments, generators, modulators and samples.
5. Pulls linked/stereo partner samples into the subset.
6. Remaps instrument/sample IDs and sample addresses into a new valid RIFF `sfbk` file.

This means the exact selected Program set can differ between source banks because their sample costs differ. Every output has a JSON manifest listing the exact retained presets and SHA-256 hashes.

## Usage

```bash
python tools/sf2_subset/sf2_subset.py input.sf2 output.sf2 \
  --target-mib 6 \
  --manifest output.json
```

## Current limits

- SF2 `smpl` / 16-bit sample payloads are supported.
- `sm24` / 24-bit SF2 rebuilding is rejected for now.
- SF3/Vorbis is not handled by this tool.
- The tool preserves modulator records but does not reinterpret them.

The generated subsets are test fixtures. They must retain the license/attribution requirements of their source SoundFonts.
