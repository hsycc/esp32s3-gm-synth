# M7 CPU1 WDT triage — physical PR #17 (2026-10-22)

## Status

**Hardware FAIL, despite GitHub CI PASS.** Do not merge PR #17 or freeze M7 based on these runs. Three separate N16R8 serial captures from the same `97a74a1-dirty` image all failed in production-shaped Music Stress before the second 131072-block I2S window completed.

The three uploaded runs include multiple reboot attempts in some recording files; `task_wdt` errors and `rst:0xc` prove this is *not* merely a missing serial monitor close marker.

## Observed hardware evidence

- The clean real-SF2 131072-block I2S window completes PASS with `render_us[min/avg/max]=117/132/1035–1036`, `render_soft_overruns=0`, `backlog_healthy=1`, `HARD_GATE failure_flags=0x00000000`.
- Before the second I2S window, the normal/full-FX standalone sweep reaches 24 voices with `max_us=1278`, while the pathological-heavy DSP sweep reaches 8 stable voices, 12 unstable. Those are standalone sweeps, *not* the later mixed workload.
- The production-shaped Music Stress is armed at about device time 203.264 seconds with 121 events (before the proposed fix), including 24 simultaneous NoteOn at phase `p7` and four dynamic-filter voices on channel 5. The `p7` event is scheduled approximately 83.220 seconds after the second window begins, around device time 286.54 seconds.
- The Task WDT fires at exactly device time **292.814 seconds** across all three separate captures: ~6.27 seconds after p7 and ~5.62 seconds **before** the first scheduled CC21 update at p8.
- The watchdog reports `IDLE1 (CPU1)` missed its feed, with CPU1 currently executing `gm_i2s_qual`. Stack traces sample different parts of Voice rendering / VoicePool / global effects and never show a single repeating locked instruction. This is consistent with continuous high-priority audio work monopolizing CPU1, not proof of an endless DSP loop.
- No second-window `i2s_qual: RESULT`, `music_stress: RESULT`, `NOTE_ON_BURST`, or `CC21_FILTER` is produced. The first-window hard gate cannot qualify the second window.

## Working hypothesis, not yet a proven root cause

PR #17 inadvertently changed the prior 24-voice *normal full-FX* burst into a *mixed DSP-heavy* burst by enabling dynamic filter tables on four channel-5 sustained voices at the same p7 NoteOn. The audio task at priority 20 can leave insufficient idle time for CPU1 and eventually hit Task WDT.

No actual CC21 controller update has happened yet at the crash time. This is not evidence of a CC21 0/64/127 update bug. Stage-level worst-block logs are unavailable because they flush only after the I2S window ends.

## Targeted physical qualification correction

The PR branch now:
1. Preserves 24 simultaneous unfiltered NoteOn events at p7 by routing the last four original notes to the existing unfiltered channel 4.
2. At p8 sends three CC120 `All Sound Off` commands to retire 16 of the 24 voices, then starts exactly four filtered sustained notes on channel 5. Applies three CC21 changes 64 → 127 → 0 at p8/p9/p10. This retains the previous peak NoteOn workload while isolating the filter-specific steady-state workload.
3. Keeps the exact 128-event preloaded queue bound; `RealtimeEventQueue::tryPush` rejects any overflow. Queue semantics and existing music stress pass gates remain intact.
4. Adds a qualification-only sustained-CPU-overrun detector: after 128 consecutive measured blocks in which the **actual renderer wall time including profiler overhead plus downmix** exceeds the 1451-us block budget, abort explicitly with `i2s_qual: SUSTAINED_OVERLOAD ... qualification=FAIL`. Do *not* disable CPU1 WDT, silently drop voices, or treat abort as a pass.
5. Prioritizes WDT/panic diagnostics in the three-run log parser rather than misreporting crashes as merely incomplete monitor captures. The script rejects multiple boot sessions per run.

## Verification gate

First require Host/M7 ESP-IDF CI green on the *new exact SHA*, then perform **one** full physical boot and inspect whether the Music Stress window now completes. Only then repeat to three independent runs and compare full attribution / hard realtime metrics.

If this still fails, retain the original logs and the fail-fast log, and investigate actual p7/p8 `VoicePool::renderAddWithEffects`, dynamic lowpass cost and I2S producer timing separately. Do not claim 24-voice normal capacity extends to arbitrary mixtures of filtered/heavy SoundFont voices.

**No change** to production DMA depth, realtime guaranteed polyphony, or global Crossfade in this triage batch.
