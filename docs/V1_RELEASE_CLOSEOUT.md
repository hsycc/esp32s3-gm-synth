# V1 source integration and release closeout

Status: **M0–M8 integrated development baseline; V1 NOT RELEASED**.

## Verified evidence

- M7 PR #17 merged into `agent/m6-sf2-playback-prepare-v1` as `4ebb8ffb`.
- M8 PR #18 merged into the same integration branch as `ed4d92bf`.
- M8 frozen hardware checkpoint `bb31908b`: 3/3 long-run passes, Clean SF2 and Music Stress 2/2 I2S hard gates clear on each run, 128/128 MIDI events, three CC21 changes, 24 stable normal/full-FX sample voices, eight stable heavy-DSP voices, PCM checksum `fe69915701738934`, no recorded WDT/panic, backlog recovered to zero. See `benchmarks/M8_PHYSICAL_THREE_RUN_2026-10-24.md`.
- The tested banner was `bb31908-dirty`. The user explicitly confirmed local modifications do not affect the qualification outcome and accepted this for the integration decision. Preserve the original banner and evidence; do not claim byte-for-byte identity with a clean CI-built ELF.
- The integrated branch must pass its own CI and the `main` integration PR checks before merging.

## Main integration gate

1. Update stale milestone documentation, confirm that `main` has not diverged from the integration branch, and open an explicit PR `agent/m6-sf2-playback-prepare-v1` → `main`.
2. Require all relevant Host, realtime, SF2/GM128, QEMU and ESP-IDF build checks to succeed at the **final integration HEAD**. Investigate any failure; do not borrow green checks from older commits.
3. Prefer preserving complete history for this accumulated M0–M8 integration. Do not squash hundreds of development commits.
4. After merge, verify `main` contains the expected integrated SHA and check any fresh push-triggered CI. Main integration is a code milestone, not authorization to distribute a V1 firmware image.

## Remaining V1 release gates

- **Audible:** document representative piano/sustain, drums, reverb/chorus, CC21 and sample-loop playback on physical hardware. Use `docs/V1_AUDIBLE_ACCEPTANCE.md`. Items without observations remain NOT RUN.
- **Loop seam:** make separate audible decisions for GeneralUser samples 604/602/62. No global crossfade is enabled or approved just because a PCM seam metric improved.
- **Output level:** unity post-FX master / ES8311 DAC -12 dB are the performance baselines; a -6 dB listening/headroom trial is a distinct image and must not substitute for the M8 timing image.
- **Compatibility:** report the exact tested native SF2 banks/presets and known unsupported cases. Current hardware output is mono-downmixed ES8311, not true spatial-stereo qualification. 24 voices means sample voices, not polyphonic notes in every layered bank.
- **Licenses:** review code/SoundFont redistribution rights, including GPL FM/GM Open, GeneralUser license obligations, and the FluidR3 LFS artifact. Tested fixtures do not automatically authorize bundling in a product.
- **Release traceability:** freeze the release revision, build config/environment, full hashes, bundled assets/license notices and tested hardware, and retain reproducible results before making public claims.

## Optional diagnostic evidence tool

`tools/release_evidence.py` can capture existing build files and Git state for troubleshooting. It is not required again merely because an accepted historical test image reports `-dirty`. Do not overwrite the original logs or silently alter the recorded version string.
