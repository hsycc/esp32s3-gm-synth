# Prepared-loop candidate review at 2b2ed3c0

## Evidence and scope

Reviewed the actual GitHub Actions artifact from run `40205294520`, artifact
`12603482126`, source HEAD `2b2ed3c0dac2f527a57b2a453ec981d36f820592`.
The ZIP SHA-256 is
`6eba2d289e8ee141812751c523dd9f33df59fa1f0efe444aaa9c1c860c7310bc`.
The bank is the existing pinned GeneralUser GS asset, SHA-256
`9575028c7a1f589f5770fccc8cff2734566af40cd26ed836944e9a5152688cfe`.

This review does not edit the policy, select a firmware fade, approve audible
quality, or merge PR #20. All runtime fade values remain zero. No WAV/PCM or
SoundFont is redistributed by this review.

The production exporter prepared 287 programs / 12,618 Zones, with no rejected
programs, and deduplicated them into 1,170 effective geometries. Of these, 160
are non-looping and 58 have NoteOn address modulation: the 218 context exclusions
in this bank are not stereo or resource-limit failures. There are 952 remaining
eligible geometries. The stratified 128 analyzed layouts produced 103
DIRECT_NO_BENEFIT, 21 DIRECT_REVIEW and 4 HOST_CANDIDATE; 824 were NOT_SAMPLED.
These layouts differ from the previous raw-shdr survey. Do not interpret
19 raw-header candidates versus 4 prepared-loop candidates as a measured error
rate improvement.

## Four candidates

Bank/Program numbers below are the exporter IDs (zero-based Program values).
Multiple Zone references can share the same effective geometry.

| Sample | Name | First Bank/Program/Zone | Frames | Seam-best | Selected | Direct seam | Selected seam | Spectral | Lag |
|---:|---|---|---:|---:|---:|---:|---:|---:|---:|
| 669 | Synth Bell-3 | 0/88/58 | 12199 | 64 | 64 | 14787 | 59 | 0.16363 | 0 |
| 657 | StrLoop - F#4 | 1/48/64 | 30457 | 64 | 16 | 3017 | 986 | 0.14722 | 0 |
| 485 | Pipe Organ-G#3 | 0/19/4 | 47838 | 16 | 16 | 9408 | 847 | 0.17049 | 0 |
| 337 | Helicopter | 0/125/0 | 12899 | 32 | 32 | 6030 | 188 | 0.07533 | 0 |

Sample 657 is the useful alternate-length case: 64f reduces the seam to 53,
but its local lag shift is -41; 16f reduces it to 986 with no current rejection
reasons. 32f is also eligible (seam 1043). The existing seam-minimizing ordering
therefore selects 16f. This is not an estimate of a -41-frame musical pitch
change, and no threshold was tuned to force the outcome.

All four are still Host candidates. In particular, a good seam metric is not
proof that Synth Bell, strings, organ or helicopter playback sounds better.

## Twenty-one Direct review cases

The following buckets are mutually exclusive per geometry, not unique Sample ID:

| Bucket | Count | Interpretation |
|---|---:|---|
| Every benefit-qualified alternative hits the period limit | 14 | Retain Direct under the present overlap/resume algorithm |
| At least one alternative is blocked only by local lag | 3 | 390, 260, 605; estimator uncertainty, not a proved pitch defect |
| At least one alternative is blocked only by neighborhood peak | 2 | 497, 806; inspect waveform context before any policy relaxation |
| At least one alternative is blocked only by neighborhood RMS | 1 | 114; local activity rises, not automatically an audible defect |
| Combined risks on every benefit-qualified alternative | 1 | 474; no presently acceptable alternative |

The 14 period-blocked geometries have Sample IDs 844, 824, 879, 470, 875,
861, 311, 511, 846, 874, 823, 875, 307, 879. Repeated IDs are distinct effective
playback geometries. Keep intentional short waveform cycles unmodified rather
than lowering the guard merely to increase the number of crossfade proposals.

Across benefit-qualified alternatives (not mutually exclusive), 16/21 review
layouts encounter period rejection, 7 local lag, 6 neighborhood peak, 5 local
spectral distance, and 3 neighborhood RMS. These counts must not be summed as
independent cases.

For Sample 497 at 16f, near-peak rises 4977 -> 5408. For 806 at 64f it rises
43494 -> 46640. For 114 at 32f, near-RMS rises about 857.59 -> 969.41 despite
seam 2510 -> 383. Do not label them audible failures from these metrics alone.

## Coverage gap in the original three references

All eight prepared geometries associated with Samples 604, 602 and 62 were
NOT_SAMPLED in the 128-layout run. The separately rerun legacy three-reference
planner still reports 604=32f and 602/62=Direct, but that is not a result of the
new risk policy on the production-prepared Zones.

In particular, 604 has three loop geometries (31,455; 7,943; and 42 frames).
The audition used the 7,943-frame geometry, whose first reference is Bank 0 /
Program 97 / Zone 0. A single `Sample604=32f` mapping would incorrectly apply
the same choice to the 42-frame variant.

## Batch added with this review

The dedicated workflow now reruns the **same policy** with `--limit 4096` to
cover all eligible GeneralUser geometries (the existing frame/context budget
is unchanged). A report-only audit compares every original analyzed/context
result for exact equality, requires no remaining NOT_SAMPLED layout, and
extracts every actual prepared geometry for the three explicit reference IDs.
The IDs are comparison requests in CI, not hardcoded policy rules.

`tools/v1_loop_review.py` preserves all alternatives and reasons for the
original four candidates / 21 reviews and produces JSON/Markdown. It rejects
mismatched source/policy/exporter identities, changed prior results, inconsistent
counts, duplicate keys, missing reference coverage, and enabled runtime fades.
Twelve added tests run in the existing dedicated workflow (50 loop tests total).

Full-bank counts and reference-policy outcomes must be read from this batch's
new CI evidence, not inferred from the earlier 128-layout report. No change to
firmware/DSP, the `prepared-loop-review-v2` thresholds, or the accepted M8 image
is made by this batch.
