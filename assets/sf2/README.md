# Public SF2 Test Assets

This directory is reserved for public SoundFont compatibility fixtures and their derived test subsets.

## Sources

### FM/GM Open SoundFont

- Upstream: `zeittresor/opensoundfont`
- Source file: `FM_GM_SoundFont_v0_2_1_mini.sf2`
- License: GPL-3.0-only
- Purpose: simple/open GM128 baseline and parser/residency tests

### GeneralUser GS

- Upstream: `mrbumpy409/GeneralUser-GS`
- Source file: `GeneralUser-GS.sf2`
- Version currently targeted by upstream documentation: 2.0.3
- License: GeneralUser GS License v2.0
- Purpose: real-world generator/modulator compliance baseline

### FluidR3_GM

- Upstream/mirror used for reproducible fetch: `pianobooster/fluid-soundfont`, release `v3.1`
- Source file: `FluidR3_GM.sf2`
- License: MIT
- Purpose: large classic GM SoundFont and partial-residency stress baseline

## Layout

```text
assets/sf2/
├── full/       # original upstream .sf2 binaries, stored through Git LFS when available
├── subsets/    # deterministic ~6 MiB derived .sf2 test fixtures
├── manifests/  # SHA-256, selected presets and source identity
└── licenses/   # upstream license/attribution texts
```

The derived subsets remain under the source SoundFont's own license terms. They are not a new project-specific asset format.

## Why Git LFS

SoundFonts are binary assets. FluidR3_GM is about 148 MB and exceeds GitHub's ordinary 100 MB per-file Git limit, so full public test assets belong in Git LFS rather than normal Git history.

The build workflow commits the ~6 MiB subsets first, then attempts the much larger full-source LFS assets separately. If account/repository LFS policy blocks the full assets, the reproducible download URLs and subset generation workflow remain usable.
