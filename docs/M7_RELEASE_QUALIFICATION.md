# M7 Release Qualification Checkpoint

Software/release-image checkpoint: `22b48b2f8ef58d01c3c0c39ddea47c7d40f330a9`

Physical long-run evidence: `benchmarks/M7_PHYSICAL_LONGRUN_2026-10-20.md`.

M7 remains **ACTIVE** while representative audible-release observations and the planned compatibility extensions are still open. The automated N16R8 physical long-run itself is now closed PASS.

## Software release matrix

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

## Large-program boundary

The current release runtime keeps `PlaybackProgramSnapshot::kMaxZones=256`; this is a realtime linear-scan bound and must not simply be raised to 720 to improve import percentages.

Measured rejected large-program shapes are:

- GeneralUser GS: 4 programs, up to 320 zones; maximum 25 key candidates and 5 key+velocity matches.
- FluidR3 GM: 12 programs, up to 720 zones; maximum 36 key candidates and 8 key+velocity matches.

The next compatibility stage therefore targets a compact caller-owned key index prepared outside the audio domain. The intent is to make NoteOn inspect only the key's candidate span rather than linearly scanning every zone while preserving the existing READY/commit/ACK lifetime contract.

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

1. Extend only the real-bank custom-modulator routes that can be represented with correct SF2 semantics; keep unsupported routes explicit rather than silently dropping them.
2. Replace the >256-zone linear NoteOn boundary with a compact key-indexed snapshot so the observed 320–720-zone presets remain bounded in realtime.
3. Investigate the remaining invalid-loop and scale-tuning special cases after those two broad compatibility classes are closed.
4. Record representative audible observations separately.
5. Re-run an appropriate physical regression after realtime-core compatibility changes, then freeze final public compatibility/polyphony wording.
