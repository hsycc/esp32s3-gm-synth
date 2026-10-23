# M7 physical timing attribution and CC21 stress checkpoint

Status: **CI / draft PR #17**. Hardware evidence must be collected on ESP32-S3 N16R8; do not call this PASS until three device logs from this exact build are checked. Formal branch stays on last qualified commit until CI and physical evidence close.

## 1. Exactly one image and three independent runs

Use the commit of PR #17 only (record the full SHA). Do not compare measurements from different revisions as run-to-run noise.

```bash
git fetch origin
git switch agent/m7-burst-cc21-physical-profile
git pull --ff-only
git rev-parse HEAD
. "$HOME/esp/esp-idf/export.sh"
bash scripts/build_m7_release_idf.sh
idf.py -B build-m7-release -p <PORT> flash
```

Power/reset and capture **three separate complete boot logs**, including the App version through `Returned from app_main()`. Run in a terminal:

```bash
script -q m7-burst-run1.log
idf.py -B build-m7-release -p <PORT> monitor
# press Ctrl+] after all PASS/failure lines, then type exit
```

Repeat for `run2` and `run3` after separate board resets; do not append them into one logfile. A complete boot executes two 131072-block I2S qualification windows. Flash only once if the image is unchanged.

```bash
python tools/m7_physical_three_run.py \
  m7-burst-run1.log m7-burst-run2.log m7-burst-run3.log \
  --output reports/m7-physical-three-run
```

This creates Markdown/JSON, checks that all App versions match, both I2S phases exist, no WDT or panic occurs, hard gates are zero, final backlog is zero, stress/polyphony PASS, and CC21 sweep plus 24-NoteOn stage attribution exist. **Soft overruns stay visible**.

## 2. What the new logs mean

- `i2s_worst: STAGES` measures setup + event preparation, event callback dispatch, Voice rendering, and FX/final output conversion, **only for blocks with 8+ scheduled events**. Values are approximate diagnostic timing; extra timers slightly perturb the measured render and should not be directly compared with uninstrumented builds.
- `music_stress: NOTE_ON_BURST` is the 64-frame bucket with the greatest number of NoteOn events and provides summed `ZoneResolver` versus `VoicePool::start` durations. It is correlated by block frame with the `i2s_worst: BLOCK` line.
- `music_stress: CC21_FILTER` documents three actual CC21 changes (64/127/0) on synthetic channel 5, with four sustained voices and `note_on_filter.cc21_to_cutoff_cents=-2400`. This tests physical code-path costs **not** GeneralUser listening or source-bank modulation correctness.
- `i2s_qual: REALTIME_HEALTH` and `HARD_GATE` remain the authoritative hard DMA/backlog/timeout checks.

## 3. Decision threshold

Maintain a hard-gate pass and healthy DMA backlog in **all three** runs. Compare worst render, maximum backlog, marginal DMA reserve and per-stage burst costs across runs. Do not change I2S DMA depth or declare lower guaranteed polyphony merely because there are isolated 1451-us soft overruns. Investigate consistently rising event/voice costs or reduced physical DMA reserve and then isolate root cause in a separate branch.

The production-shaped physical workload in this PR includes additional filter work, so use the pre-change `11283e04` data as context only, not a perfectly matched A/B cycle measurement. A true optimization regression comparison requires paired builds with the same instrumentation and fixed workload.

## 4. Not included

No loop Crossfade change, no new SF2 compatibility semantics, no UI/Wi-Fi concurrency stress, no actual GeneralUser CC21 physical audition, and no assertion about ESP32-S3 true stereo imaging from mono ES8311 output.
