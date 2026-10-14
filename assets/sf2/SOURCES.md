# SoundFont Source Pins

These public assets are fetched by `.github/workflows/sf2-assets.yml` and stored with Git LFS when repository/account policy permits.

## FM/GM Open SoundFont

- Upstream: `https://github.com/zeittresor/opensoundfont`
- Pinned commit: `5a20775a9c1b874effdaf1d131862748fdd135ba`
- File: `FM_GM_SoundFont_v0_2_1_mini.sf2`
- License file: `LICENSE`
- License: GPL-3.0-only

## GeneralUser GS

- Upstream: `https://github.com/mrbumpy409/GeneralUser-GS`
- Pinned commit: `684543d5e5efaef08d02be50dcda8d552478fa60`
- File: `GeneralUser-GS.sf2`
- Known upstream SHA-256 for v2.0.3: `9575028c7a1f589f5770fccc8cff2734566af40cd26ed836944e9a5152688cfe`
- License file: `documentation/LICENSE.txt`
- License: GeneralUser GS License v2.0

## FluidR3_GM

- Upstream mirror: `https://github.com/pianobooster/fluid-soundfont`
- Release: `v3.1`
- File: `FluidR3_GM.sf2`
- Release asset size: 148,398,306 bytes
- License file: `COPYING`
- License: MIT

## Derived subsets

Each `assets/sf2/subsets/*.sf2` file is rebuilt from one source SoundFont by `tools/sf2_subset/sf2_subset.py` with a nominal 6 MiB target. Exact content and SHA-256 are recorded in `assets/sf2/manifests/`.

The subsets are not intended as replacement GM banks. They are reproducible parser/resolver/residency fixtures and retain their source SoundFont's license terms.
