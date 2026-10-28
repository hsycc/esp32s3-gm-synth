# V1 source integration and release closeout

Status: **M0–M8 and optional serial Loop qualification merged to main;
concurrent Loop hardware/audible gates open; V1 NOT RELEASED**.

## Verified evidence

- M7 PR #17 merged into `agent/m6-sf2-playback-prepare-v1` as `4ebb8ffb`.
- M8 PR #18 merged into the same integration branch as `ed4d92bf`.
- M8 frozen hardware checkpoint `bb31908b`: 3/3 long-run passes, Clean SF2 and Music Stress 2/2 I2S hard gates clear on each run, 128/128 MIDI events, three CC21 changes, 24 stable normal/full-FX sample voices, eight stable heavy-DSP voices, PCM checksum `fe69915701738934`, no recorded WDT/panic, backlog recovered to zero. See `benchmarks/M8_PHYSICAL_THREE_RUN_2026-10-24.md`.
- The tested banner was `bb31908-dirty`. The user explicitly confirmed local modifications do not affect the qualification outcome and accepted this for the integration decision. Preserve the original banner and evidence; do not claim byte-for-byte identity with a clean CI-built ELF.
- PRs #19–#24 subsequently merged into main. PR #25 merged with full history as `51dc57d42057e9c23d3cf593717f56cc3841c92e`, after live review of unchanged head `5502e974` / base `6146cc34`: 17/17 workflows and 25/25 check runs SUCCESS, no review submissions or unresolved threads. Its accepted QEMU artifacts cover serial on/off startup, not concurrent I2S or physical performance.

## Main integration gate — completed

The previous integration checklist is complete through #25. Preserve the old
commits and actual historical evidence. Each new integration batch still needs
its own final-HEAD Host/target checks and fresh main push verification; the
startup workflow is PR/dispatch-only, so the main trigger set differs.
Main integration is a code milestone, not authorization to distribute V1.
The five workflows triggered by the #25 merge push completed successfully;
the default ESP-IDF run is `40636633886`.

## Remaining V1 release gates

- **Audible:** document representative piano/sustain, drums, reverb/chorus, CC21 and sample-loop playback on physical hardware. Use `docs/V1_AUDIBLE_ACCEPTANCE.md`. Items without observations remain NOT RUN.
- **Concurrent Loop hardware:** qualify actual control/audio overlap, internal RAM/largest block, unchanged 4.5 MiB PCM capacity, Prepare/hash/load/analyze latency, real active Loop Voices, I2S/DMA/queue health and stable normal/heavy polyphony on the new on/off images. The old 24/8 result cannot satisfy this gate. See `docs/V1_LOOP_REALTIME_INTEGRATION.md`.
- **Loop seam:** retain the user's existing Host preference: 604/32f sounded best; 602 and 62 had little difference, with no forced-processing preference. The frozen automatic policy remains authoritative for effective Zone ranges (604/7943f chooses32f; its other ranges and the exported602/62 references remain Direct). This does not approve global enablement or physical full-instrument listening.
- **Output level:** unity post-FX master / ES8311 DAC -12 dB are the performance baselines; a -6 dB listening/headroom trial is a distinct image and must not substitute for the M8 timing image.
- **Known digital headroom failure:** the new fixed FM Program16 / 24-Voice / velocity64 cohort clips in both Direct and on modes in Host rendering. It is retained as a negative qualification with the original zero-clipping gate; see `docs/V1_LOOP_REALTIME_INTEGRATION.md` for exact scene and counts. Do not call this workload a released 24-Voice audio configuration until headroom is resolved and independently verified.
- **Compatibility:** report the exact tested native SF2 banks/presets and known unsupported cases. Current hardware output is mono-downmixed ES8311, not true spatial-stereo qualification. 24 voices means sample voices, not polyphonic notes in every layered bank.
- **Licenses:** review code/SoundFont redistribution rights, including GPL FM/GM Open, GeneralUser license obligations, and the FluidR3 LFS artifact. Tested fixtures do not automatically authorize bundling in a product.
- **Release traceability:** freeze the release revision, build config/environment, full hashes, bundled assets/license notices and tested hardware, and retain reproducible results before making public claims.

## Optional diagnostic evidence tool

`tools/release_evidence.py` can capture existing build files and Git state for troubleshooting. It is not required again merely because an accepted historical test image reports `-dirty`. Do not overwrite the original logs or silently alter the recorded version string.
