# V1 dynamic loop crossfade planner — Host gate

## Scope

This branch implements **advisory, host-side** selection. It does not edit the
ESP32-S3 realtime renderer, SoundFont banks, PCM samples, or accepted M8
performance profile.

Run:

```sh
python3 -m unittest tests.test_v1_loop_crossfade_planner -v
python3 tools/v1_loop_crossfade_planner.py assets/sf2/full/GeneralUser-GS.sf2 \
  --output build/v1-loop-plans.json
```

The tool uses the existing M7 loop A/B WAV renderer's overlap semantics and
the same three qualified GeneralUser sample geometries (604, 602, 62).
The output is an advisory JSON with all candidate metrics. It **must not**
silently create a firmware crossfade map.

## Policy

- Direct remains the fallback.
- Candidates are 16 / 32 / 64 source frames (validated to be shorter than half the loop).
- Only attempt a fade if the unprocessed seam step is at least 2500 PCM16
  counts, the improvement is at least 1800 counts and 65%, and the
  overlapping fade changes the loop period by no more than 2%.
- Select minimum candidate residual seam; prefer the shorter fade if tied.
- Report other near-seam step statistics for inspection, not as a guarantee
  of superior sound quality.
- A subjective audition remains authoritative. User's current hearing notes:
  **604 favors 32f**, whereas **602 and 62 were inconclusive / barely different**.
  These notes are an independent evaluation target, not hardcoded sample-ID
  rules.

## Important limitations and required next gates

This first-pass objective scorer does **not** analyze fundamental frequency,
spectral drift, onset artifacts, phase coherence or pitch/period side effects
beyond a bounded period-change fraction. It may disagree with the listener,
and may pick a shorter fade than 604's preferred 32f.

Before embedding into ESP32-S3:
1. Run against actual SF2 PCM and inspect each candidate's score; record any
   mismatch with observed 604/602/62 preference and refine objective metrics.
2. Add spectral/period and waveform-distortion features without overfitting
   numeric sample IDs; test diverse loops, including silent/smooth/short,
   positive/negative PCM and near-end geometry.
3. Move *analysis* to asynchronous Program Prepare/Sample Residency; publish
   a bounded immutable plan with the sample's identity/generation, as needed
   to avoid stale plans on eviction/replacement.
4. Implement the per-Voice playback fade only under an explicit validated
   plan. Preserve Direct's bit-identical legacy path and use bounded
   fixed-point computations only for a selected wrap.
5. Host PCM equivalence and restart/wrap tests, then QEMU, and finally N16R8
   24 normal-Voice + eight heavy-DSP qualified physical I2S/deadline tests.
   Compare against current accepted PCM and soft-overrun performance baselines.
   A planned deviation to loop output must be explicitly acknowledged, not
   hidden by weakening the checksum gate.

Do not enable blanket crossfade or consider a numerical seam reduction a
listening PASS.
