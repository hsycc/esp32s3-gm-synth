# V3 Host policy: source-matched peak attribution

Status: experimental Host candidates only. No firmware, renderer, PCM asset,
DMA, gain or polyphony change; no runtime approval or listening PASS.

## The defect

The v2 +/-96 output-frame windows can contain different original source edges
because overlap resumes at +fade. A larger untouched source edge moving into
the window must not be called a newly generated Crossfade spike. This was
observed on GeneralUser 604/7,943 frames and 806; 497/16f provides a countercase
whose higher peak really occurs in blended output. These IDs are evidence
references, never policy conditions.

## Correction, not a threshold increase

For every edge in the existing complete symmetric window:

- If both values are unchanged consecutive source samples, match them to that
  exact source pair. Verify both values against the reference renderer. Its
  size is not a generated-peak failure even when it was outside the Direct
  output window.
- Otherwise retain the original Direct window peak limit for the edge: this
  includes fade entry/exit, internal mixed edges and nonconsecutive joins.
- Check ALL changed edges, not just the largest peak. An untouched 62,000-count
  peak must not hide a smaller new mixed edge exceeding the original limit.
- Missing, partial, inconsistent or nonfinite attribution fails closed in
  source-matched mode. No raw-metric fallback is allowed by analyze().

All benefit, period (<1%), local DFT/lag and neighborhood RMS thresholds remain
unchanged. near_max and near_rms remain the old raw diagnostics; the added
source_matched_peak object records attribution and verified edge counts, not
PCM. The RMS gate still compares fixed output windows: this batch corrects
peak attribution only, not every possible window-composition sensitivity.

Policy version is now prepared-loop-review-v3-source-matched-peak. The version
is part of geometry identity; old and new reports are matched by full geometry,
asset, exporter and Zone references, not by assuming old plan hashes persist.
Low-level choose() retains explicit legacy-window behavior for old diagnostic
callers and comparison tests; analyze() always requests source_matched.
Neither path produces an enabled firmware plan.

## Verification

16 new tests cover multiwrap mapping, all changed edges, mixed peaks hidden
under unchanged peaks, PCM16 extremes, conservative failures and controlled
report comparison. Before publication the local new tests passed, Python
compiled, and legacy selection was checked against the saved d52bbe5e CI
report for all 952 analyzed geometries with exact candidate/reason equality.
This local check is not an ESP-IDF build or a replacement for GitHub CI.

The dedicated workflow runs all Loop tests, scans the current bank, then
checks out immutable d52bbe5e tools in an isolated build directory and reruns
v2 against the SAME bank and native exporter. No expiring artifact is needed.
The comparison requires matching full inventories, references, all original
raw metrics, numeric thresholds and non-peak rejection reasons. Only removals
of misattributed peak failures may change selection; every change is recorded.
Current CI results and final counts must be read from the produced artifact,
not inferred from the old 44 candidates or the user's 604 preference.

Runtime fade remains 0; runtime_approved remains false. A Host candidate is
not proof of unchanged pitch/timbre, listening quality or MCU timing capacity.
