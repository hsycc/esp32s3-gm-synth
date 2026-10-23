# M7 Release Qualification Checkpoint

Software/release-image checkpoint: `22b48b2f8ef58d01c3c0c39ddea47c7d40f330a9`

Physical long-run evidence: `benchmarks/M7_PHYSICAL_LONGRUN_2026-10-20.md`.

The automated N16R8 long-run for checkpoint `22b48b2f` is PASS. The later CC21/KeyIndex/loop-geometry code has passed CI, but has **not** yet been independently requalified on physical hardware; M7 audio-listening and post-change physical evidence remain open.

## Current host compatibility checkpoint — 2026-10-21

At formal development HEAD `4079d8d4` (PR #15 merged), the M7 full-bank matrix and issue diagnostics confirm all program identities in these exact checked-in SF2 assets as StrictQualified:

| Bank | Programs qualified | Rejected | Unsupported modulators | Preview zones |
|---|---:|---:|---:|---:|
| FM/GM Open full | 5760/5760 | 0 | 0 | 0 |
| GeneralUser GS full | 287/287 | 0 | 0 | 0 |
| FluidR3 GM full | 189/189 | 0 | 0 | 0 |

This covers **host Program Prepare semantics only**, not audio listening, generalized third-party SF2 files, true linked stereo, or updated DSP/I2S physical performance.

The GeneralUser six previously rejected programs contain seven continuous-loop zones where playback sample-start lies inside the old loop interval. Only this bounded case is normalized to use playback sample-start as the loop start. The original loop issue remains visible via `PlaybackIssueLoopStartClamped`.

Loop audio A/B export (PR #16) produces mono native-rate WAVs for samples 604, 602 and 62: direct / 16 / 32 / 64-frame linear overlap. PCM16 measurements from the real-bank CI:

| Sample | Direct wrap step | 16f | 32f | 64f | Interpretation |
|---:|---:|---:|---:|---:|---|
| 604 | 13665 | 12145 | 1096 | 2531 | 32f reduces immediate wrap jump, but local peaks remain |
| 602 | 1511 | 1718 | 468 | 206 | 16f makes direct wrap worse; audition 32/64f |
| 62 | 137 | 41 | 187 | 100 | already low; longer fades are not consistently better |

These are objective PCM step values, **not listening results**. Crossfading overlaps loop tail/head and advances the source position after the overlap, changing loop-period timing. No Crossfade is enabled in embedded DSP solely on these numbers.

## Historical software release matrix — 2026-10-20 checkpoint

The production `Sf2PlaybackProgramPreparer` classifies complete native SoundFonts program-by-program. Acceptance means `StrictQualified`; unsupported or malformed programs remain explicit rejects rather than being silently approximated.

| Bank | Presets | StrictQualified | Explicit reject | Main release limitations |
|---|---:|---:|---:|---|
| FM/GM Open full | 5760 | 5760 | 0 | none in the scanned bank |
| GeneralUser GS full | 287 | 243 | 44 | 36 custom-modulator programs, 4 invalid-loop programs, 4 programs above the 256-zone realtime bound |
| FluidR3 GM full | 189 | 173 | 16 | 12 programs above the 256-zone realtime bound, 3 custom-modulator programs, 1 scale-tuning program |

`gain_above_unity_clamped` remains diagnostic telemetry, but is not itself a release rejection. Negative `initialAttenuation` is retained into the realtime NoteOn summing node, where the final SoundFont attenuation clamp prevents amplification above unity.

## Stereo/sample-type boundary

M7 supports the FluidR3-style data shape where a sample is tagged `leftSample` or `rightSample` but has `sampleLink=0` and the instrument already references each side independently. The side is normalized as an independent mono sample and its authored left/right identity is folded into the base pan; the resolver does not invent a partner sample or duplicate the voice.

This is **not** a claim of general reciprocal linked-stereo support. A genuine linked sample/pair that is not representable by the current runtime remains an explicit `StereoOrLinkedSample` reject.

## Indexed large-program playback — current

`PlaybackProgramSnapshot::kMaxZones=1024`; immutable `PlaybackProgramKeyIndex` is built in Program Prepare and carried across snapshot publication/retirement. Programs above 256 zones require this index. The audio domain scans only key candidates, not all 720 zones; the synthetic 720-zone test resolves key 42 by scanning six candidates.

This resolves the prior 320–720-zone host qualification blockers without changing realtime ownership or allocating in NoteOn.

## M7.2 / robustness evidence

CI covers:

- 16-channel active playback with Program Change while published Voice references remain live;
- Prepare -> READY -> block-boundary commit -> ACK ownership transitions;
- zero stale-release/stale-residency/acquire faults on the qualified workload;
- deterministic malformed-SF2 rejection for short/truncated/invalid RIFF forms without crash;
- tracked GM128 matrix and full-bank cache/residency/runtime gates.

## Dedicated physical long-run result

`esp-idf-m7-release-build` produces a separate N16R8 image rather than modifying the established M6 hwbench baseline. The release profile is ESP32-S3 N16R8 at 240 MHz, 4.50 MiB PCM residency arena, ES8311 physical I2S, 44.1 kHz/64-frame blocks, 8 x 64-frame DMA descriptors, and `131072` measured blocks per long-run window (~190.22 s).

The 2026-10-20 hardware run completed both long windows.

Clean real-SF2 long run:

- 131072 measured blocks;
- render avg/max `122/935 us`;
- producer avg/max `126/939 us`;
- zero soft work overruns and zero backlog;
- `HARD_GATE failure_flags=0x00000000`;
- zero stalls/timeouts/partial writes/active queue overflows/clipping;
- `i2s_qual: PASS` and `clean_sf2: PASS`.

Production-shaped music stress:

- 131072 measured blocks / 118 scheduled events;
- render avg/max `737/1870 us`;
- producer avg/max `740/1874 us`;
- four transient render and producer soft overruns;
- maximum backlog `423 us` / one block, maximum recovery horizon 19 blocks;
- final backlog `0 us`;
- minimum DMA margin `11187 us` against the `11610 us` DMA window;
- backlog never crossed the DMA window;
- zero stalls/timeouts/partial writes/active queue overflows/clipping;
- `HARD_GATE failure_flags=0x00000000`;
- `music_stress: PASS`, `i2s_qual: PASS`, `effects_bench: PASS final_refs=0`.

The same run re-confirmed the established product envelope: normal/full-FX maximum 24 voices with first tested unstable step 28, and pathological-heavy maximum 8 voices with first unstable step 12.

See `benchmarks/M7_PHYSICAL_LONGRUN_2026-10-20.md` for the retained metrics and semantic counters.

## Audible evidence boundary

Subjective audible evidence stays separate from automated timing evidence.

The current Korvo/ES8311 qualification output is mono: renderer L/R is averaged before the single DAC/speaker path. Therefore this board can physically audition pitch, loop/envelope behavior, drums and FX, and can prove that stereo-derived content survives mono downmix, but it cannot prove a spatial stereo image or left/right pan by listening to its single speaker.

For M7 closure:

- retain the existing real-SF2 dry cue for pitch/envelope sanity;
- record representative sustained/loop, drum and FX listening observations;
- use host/stereo PCM evidence for left/right and pan semantics, with physical mono-downmix as the N16R8 hardware check;
- do not describe the mono Korvo listening run as a true stereo audition.

## Remaining M7 work

1. Listen to and compare the twelve PR #16 A/B files, including the pitch/timbre and periodicity effects of overlapping the loop boundaries. Do not equate improved PCM step magnitude with perceptual approval.
2. Decide per sample (604, 602, 62) whether direct loop, a **bounded** crossfade, or another authoring-side edit is appropriate. New DSP behavior requires a separate gated runtime implementation with no generic extra work for unaffected samples.
3. Build, flash and physically validate the **current post-CC21 and loop-fix image** on N16R8: 44.1kHz / 64 frames, backlog/DMA hard gate, normal/full-FX 24 voices and heavy DSP 8 voices, and the 131072-block production-shaped long run. Retain logs and exact app SHA.
4. Record real-SF2 melodic sustain/loop, drums, effects and CC21 sweep listening; ES8311 mono hardware does not prove stereo imaging. Use host stereo PCM reference for left/right.
5. Only after new hardware and listening evidence, freeze M7 release/polyphony wording and update this document's physical checkpoint.

