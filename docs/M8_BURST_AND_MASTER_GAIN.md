# M8: event-burst attribution and independent master volume

## Starting point and guardrails

- Base: PR #17, SHA `3c752b14`. M7 N16R8 real hardware supplied three independent logs of fixed `95aa189-dirty` image with identical ELF prefix `94032284d` and matching Music Stress checksum `fe69915701738934`, three complete 131072-block clean + stress phases, zero WDT/panic, zero hard-gate failures, zero final backlog and 3 CC21 filter updates. Exact local-diff/CI binary equivalence **has not been established**; leave PR #17 pending that provenance check.
- In those logs worst filtered-NoteOn block was 2286–2292 us at 1451-us soft render budget, with ~989 us spent in Event Apply. Plain 24-NoteOn peak was ~2125 us.
- This branch is **stacked**, not merged. The master and profiler changes are deliberately orthogonal. Do not alter 44.1 kHz /64-frame block, 8×64 DMA, 4.5 MiB arena, 24 normal voices, 8 heavy stable, or CC21 isolation to claim a performance win.

## Independent volume stages

1. **Per-channel musical controls**: keep MIDI CC7 and CC11 existing behavior. These are not a master control.
2. **Post-FX digital Master Gain**: `ScheduledAudioBlockRenderer::setMasterGainQ15(0..32767)`, with `32767` as **exact bypass**. Applies to complete dry+wet output immediately before final signed-int16 saturation; preserves existing PCM checksum at unity, bounds input and disallows digital amplification. `0` mutes. Must be called before render or from the audio owner at a safe block boundary; concurrent setting from another task is not supported by the non-atomic API.
3. **ES8311 DAC analog-output-layer volume control** (ES8311 digital volume register): `CONFIG_GM_SYNTH_I2S_QUAL_DAC_ATTENUATION_DB` (0..12 dB); baseline remains **-12 dB / reg 0xA7**; -6 dB becomes 0xB3 and 0 dB 0xBF. This qualification-only configuration is read/written on control thread during codec startup, with readback gate and accurate `volume_db=-N` log. Never I2C in the audio task.
4. **PA**: GPIO48 remains only enable; this PR does not infer a variable analog amplifier gain.

Keep DAC at -12 dB and Master at unity for any M7-vs-M8 speed comparison. Loudness trials at -9/-6 dB are separate controlled images; check clipping and speaker safety. Note the current physical stress fixture has `kStressGainQ15=8192` (~25% amplitude) on top of DAC -12 dB.

## Event-heavy diagnostic instrumentation

Only when worst-block profile is armed on physical ESP and an event batch has >=8 events:
- Maintain existing `setup_prepare_us`, `event_apply_us`, `voice_render_us` and `effects_finalize_us`.
- Add `note_on_apply_us`, `cc120_apply_us` (All Sound Off), `other_cc_apply_us` (including CC21), and `other_event_apply_us`.
- These subtype sums are within Event Apply but include an additional timer read per event; they are **diagnostic-only** and not an uninstrumented performance regression baseline. Ranking/profiling still depends on the existing render measurement. No allocation, logging, file I/O, or I2C inside render.
- Correlate Stage buckets against `i2s_worst: BLOCK` event counts and `music_stress: NOTE_ON_BURST` aggregate to identify whether the next fast path belongs in channel controller refresh, static/dynamic filter table preparation, or VoicePool start.

## CI and physical acceptance

- All host tests, SF2 real-bank / M7 matrix, QEMU/ESP builds must pass after the **final** M8 commit. Host test demonstrates exact unity, half-volume, mute, rejected invalid gain, and unchanged scheduler sequencing.
- First physical run with Master unity and DAC -12 dB: require same master checksum as prechange `fe69915701738934`, all 128 MIDI events applied, CC21 changes >=3, zero hard-gate flags, no WDT, final backlog 0, and full `Returned from app_main()`.
- Compare new Event Apply subtype timings cautiously; categorize time cost first, then implement real semantic-preserving optimizations in a subsequent testable change. Do not optimize by merely moving test events into separate blocks.
- Only after stable default configuration test, consider -6 dB DAC loudness listening trial as a **separate** run. Qualify actual output clip/gain, not just software clipping flags.

## Risk and order

1. PR #17 source/binary provenance check; do not silently merge when `-dirty` content unknown.
2. M8 Host+IDF validation and master unity determinism.
3. One full N16R8 run for subtype timing.
4. Code optimization of identified dominant cost, with A/B checksum, timing and hard-gate validation.
5. DAC loudness trial; external PA/speaker clipping still needs listening or analog measurement.

Current M8 is a functional volume-control and diagnostic stage, **not yet proven to reduce the 2292-us worst-case**.
