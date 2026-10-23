# M8: event-burst attribution and independent master volume

## Starting point and guardrails

- Base: PR #17, SHA `3c752b14`. M7 N16R8 real hardware supplied three independent logs of fixed `95aa189-dirty` image with identical ELF prefix `94032284d` and matching Music Stress checksum `fe69915701738934`, three complete 131072-block clean + stress phases, zero WDT/panic, zero hard-gate failures, zero final backlog and 3 CC21 filter updates. Exact local-diff/CI binary equivalence **has not been established**; leave PR #17 pending that provenance check.
- In those logs worst filtered-NoteOn block was 2286–2292 us at 1451-us soft render budget, with ~989 us spent in Event Apply. Plain 24-NoteOn peak was ~2125 us.
- This branch is **stacked**, not merged. The master and profiler changes are deliberately orthogonal. Do not alter 44.1 kHz /64-frame block, 8×64 DMA, 4.5 MiB arena, 24 normal voices, 8 heavy stable, or CC21 isolation to claim a performance win.

## Independent volume stages

1. **Per-channel musical controls**: keep MIDI CC7 and CC11 existing behavior. These are not a master control.
2. **Post-FX digital Master Gain**: `ScheduledAudioBlockRenderer::setMasterGainQ15(0..32767)`, with `32767` as **exact bypass**. Physical builds also expose `CONFIG_GM_SYNTH_I2S_QUAL_MASTER_GAIN_Q15`, applied before the I2S run starts. The independent 441-Hz reference tone bypasses synth master. Applies to complete dry+wet output immediately before final signed-int16 saturation; preserves existing PCM checksum at unity, bounds input and disallows digital amplification. `0` mutes. Must be called before render or from the audio owner at a safe block boundary; concurrent setting from another task is not supported by the non-atomic API.
3. **ES8311 DAC analog-output-layer volume control** (ES8311 digital volume register): `CONFIG_GM_SYNTH_I2S_QUAL_DAC_ATTENUATION_DB` (0..12 dB); baseline remains **-12 dB / reg 0xA7**; -6 dB becomes 0xB3 and 0 dB 0xBF. This qualification-only configuration is read/written on control thread during codec startup, with readback gate and accurate `volume_db=-N` log. Never I2C in the audio task.
4. **PA**: GPIO48 remains only enable; this PR does not infer a variable analog amplifier gain.

Keep DAC at -12 dB and Master at unity for any M7-vs-M8 speed comparison. Loudness trials at -9/-6 dB are separate controlled images; check clipping and speaker safety. Note the current physical stress fixture has `kStressGainQ15=8192` (~25% amplitude) on top of DAC -12 dB.

## Event-heavy diagnostic instrumentation

Only when worst-block profile is armed on physical ESP and an event batch has >=8 events:
- Maintain existing `setup_prepare_us`, `event_apply_us`, `voice_render_us` and `effects_finalize_us`.
- Add `note_on_apply_us`, `cc120_apply_us` (All Sound Off), `other_cc_apply_us` (including CC21), and `other_event_apply_us`.
- These subtype sums are within Event Apply but include an additional timer read per event; they are **diagnostic-only** and not an uninstrumented performance regression baseline. Ranking/profiling still depends on the existing render measurement. No allocation, logging, file I/O, or I2C inside render.
- Correlate Stage buckets against `i2s_worst: BLOCK` event counts and `music_stress: NOTE_ON_BURST` aggregate to identify whether the next fast path belongs in channel controller refresh, static/dynamic filter table preparation, or VoicePool start.

## Actual product-path peak optimization

For a SoundFont zone with a dynamic filter range whose minimum or maximum
offset is zero, the generated 9-point coefficient table *already contains*
the base `initialFilterFc/Q` biquad as one endpoint. The resolver previously
recomputed that identical base coefficient set separately on each NoteOn.
M8 now reuses the exact first/last coefficient entry and falls back to the
previous standalone computation if neither endpoint is zero or no valid
dynamic table exists. This changes no sample/key/velocity dependence,
controller timeline or pitch/filter interpolation; the regression test
compares all Q24 coefficient fields against independent reference
`prepareResonantLowpassQ24` for three notes.

The physical CC21 fixture with `cc21_to_cutoff_cents=-2400` should take
this exact-match path. It is a small general-purpose fast path for real
SoundFont zones; do not claim a measured microsecond win without new
instrumented N16R8 logs.

## CI and physical acceptance

- All host tests, SF2 real-bank / M7 matrix, QEMU/ESP builds must pass after the **final** M8 commit. Host test demonstrates exact unity, half-volume, mute, rejected invalid gain, and unchanged scheduler sequencing.
- First physical run with Master unity (`CONFIG_GM_SYNTH_I2S_QUAL_MASTER_GAIN_Q15=32767`) and DAC -12 dB (`CONFIG_GM_SYNTH_I2S_QUAL_DAC_ATTENUATION_DB=12`): require same master checksum as prechange `fe69915701738934`, all 128 MIDI events applied, CC21 changes >=3, zero hard-gate flags, no WDT, final backlog 0, and full `Returned from app_main()`.
- Compare new Event Apply subtype timings cautiously; categorize time cost first, then implement real semantic-preserving optimizations in a subsequent testable change. Do not optimize by merely moving test events into separate blocks.
- Only after stable default configuration test, consider -6 dB DAC loudness listening trial as a **separate** run. Qualify actual output clip/gain, not just software clipping flags.

## Risk and order

1. PR #17 source/binary provenance check; do not silently merge when `-dirty` content unknown.
2. M8 Host+IDF validation and master unity determinism.
3. One full N16R8 run for subtype timing.
4. Code optimization of identified dominant cost, with A/B checksum, timing and hard-gate validation.
5. DAC loudness trial; external PA/speaker clipping still needs listening or analog measurement.

Current M8 is a functional volume-control and diagnostic stage, **not yet proven to reduce the 2292-us worst-case**.

## M8 N16R8 physical A/B execution (frozen HEAD `bb31908bf4af5c966e2f17075e54b08d9a3d057c`)

At this checkpoint the full 13/13 GitHub Actions suite passed. Freeze this
SHA for physical acceptance: do not mix it with subsequent M8 commits and do
not use an unreviewed `-dirty` image as a release evidence artifact.

1. Use a **new detached worktree** to preserve any changes in the existing
   developer checkout:

   ```sh
   cd ~/work/esp32s3-gm-synth
   git fetch origin
   git worktree add --detach ../esp32s3-gm-synth-m8-physical bb31908bf4af5c966e2f17075e54b08d9a3d057c
   cd ../esp32s3-gm-synth-m8-physical
   test -z "$(git status --porcelain)" || { echo "DIRTY WORKTREE"; exit 1; }
   git rev-parse HEAD
   ```

2. Build with the previously installed **ESP-IDF 6.1** environment:

   ```sh
   bash scripts/build_m7_release_idf.sh
   grep -E '^CONFIG_GM_SYNTH_I2S_QUAL_(MASTER_GAIN_Q15|DAC_ATTENUATION_DB)=' sdkconfig.m7release
   ```

   Require `MASTER_GAIN_Q15=32767` and `DAC_ATTENUATION_DB=12`.
   Retain 44.1 kHz, 64 frames, eight 64-frame DMA descriptors, 4.5 MiB
   PCM arena, 24 Full-FX voices and eight Heavy-DSP stable voices. Record the
   ELF SHA-256 and full commit ID along with the runtime App version:

   ```sh
   mkdir -p reports/m8-physical
   shasum -a 256 build-m7-release/*.elf | tee reports/m8-physical/elf-sha256.txt
   git rev-parse HEAD > reports/m8-physical/source-commit.txt
   ```

3. Flash once using the actual detected device path (example below), then
   record an **independent boot per run**. Exit the monitor with Ctrl+].
   Do not reset again after the monitor has already initiated a boot.

   ```sh
   python "$IDF_PATH/tools/idf.py" -B build-m7-release -p /dev/cu.usbmodem1101 flash
   script -q reports/m8-physical/m8-run1.log /bin/zsh -c 'python "$IDF_PATH/tools/idf.py" -B build-m7-release -p /dev/cu.usbmodem1101 monitor'
   ```

   If Run 1 passes, repeat fresh monitor/boot captures as
   `m8-run2.log` and `m8-run3.log` with the same untouched ELF.

4. Per-run acceptance gates:
   - Runtime App version matches the frozen commit and has no unreviewed
     `-dirty` suffix; record matching ELF SHA for all three runs.
   - Complete clean-SF2 and music-stress I2S qualifications both PASS, with
     both HARD_GATE flags exactly `0x00000000`.
   - Music-stress PCM checksum remains `fe69915701738934`, all 128 MIDI
     events are processed, CC21 updates >=3, no Task WDT/panic, final
     backlog zero and `Returned from app_main()` present.
   - Compare Top-N worst Block, `event_apply_us`, `note_on_apply_us`,
     `other_cc_apply_us`, `voice_render_us`, soft overruns and
     DMA minimum margin to the M7 three-run baseline. Peak reduction is
     a **measured** result, never inferred from CI.
   - Do not merge PR #17 until the older `95aa189-dirty` firmware's
     local diffs have been examined and provenance documented.

5. Collect key diagnostic lines without suppressing the original logs:

   ```sh
   grep -E 'App version:|i2s_qual: (PASS|HARD_GATE|SUSTAINED_OVERLOAD)|i2s_worst: (SUMMARY|STAGES|BLOCK)|music_stress: (RESULT|NOTE_ON_BURST|CC21_FILTER)|task_wdt:|Guru Meditation|Returned from app_main' reports/m8-physical/m8-run1.log
   ```

6. Loudness trials are **separate** from baseline timing runs. Only after
   the unity/-12-dB image passes, build a second controlled image using a
   separate SDKCONFIG defaults override with
   `CONFIG_GM_SYNTH_I2S_QUAL_DAC_ATTENUATION_DB=6`, retain master unity,
   and record the altered image hash. Check the actual speaker and DAC
   outputs for audible/analog clipping. Never compare its timing/PCM evidence
   as if it were the original frozen image.

Any failed hard gate, WDT, inconsistent checksum or dirty-image provenance is
a stop condition, not an invitation to quietly tune DMA buffers or loosen
acceptance limits.
