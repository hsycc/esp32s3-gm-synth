# M7 Release Qualification Checkpoint

Checkpoint: `22b48b2f8ef58d01c3c0c39ddea47c7d40f330a9`

M7 remains **ACTIVE** until the dedicated physical long-run and audible-release sampling are completed. This document freezes the software/CI evidence so the remaining hardware work cannot silently redefine compatibility claims.

## Software release matrix

The production `Sf2PlaybackProgramPreparer` now classifies complete native SoundFonts program-by-program. Acceptance means `StrictQualified`; unsupported or malformed programs remain explicit rejects rather than being silently approximated.

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

The release runtime deliberately keeps `PlaybackProgramSnapshot::kMaxZones=256`; this is also the realtime NoteOn scan bound and must not be raised merely to improve import percentages.

Measured rejected large-program shapes are:

- GeneralUser GS: 4 programs, up to 320 zones; maximum 25 key candidates and 5 key+velocity matches.
- FluidR3 GM: 12 programs, up to 720 zones; maximum 36 key candidates and 8 key+velocity matches.

These measurements point toward a future compact key-indexed snapshot rather than a 720-entry linear realtime scan. The programs remain explicit release rejects in M7.

## M7.2 / robustness evidence

CI also covers:

- 16-channel active playback with Program Change while published Voice references remain live;
- Prepare -> READY -> block-boundary commit -> ACK ownership transitions;
- zero stale-release/stale-residency/acquire faults on the qualified workload;
- deterministic malformed-SF2 rejection for short/truncated/invalid RIFF forms without crash;
- tracked GM128 matrix and full-bank cache/residency/runtime gates.

## Dedicated physical long-run image

`esp-idf-m7-release-build` produces a separate N16R8 image rather than modifying the established M6 hwbench baseline.

Release profile:

- ESP32-S3 N16R8, 240 MHz;
- 4.50 MiB PCM residency arena;
- ES8311 / Korvo physical I2S path;
- 44.1 kHz, 64-frame renderer blocks;
- 8 x 64-frame DMA descriptors (~11.61 ms configured DMA window);
- `131072` measured blocks = about `190.22 s` continuous measured streaming;
- production-shaped sustain/release/percussion/pitch/CC/Program-Change/GM-Reset phases distributed across the full measured interval;
- the same backlog-aware hard realtime model closed in M6.

The build artifact also carries `tools/m7_release/physical_report.py`. A captured serial log is qualified with:

```bash
python3 tools/m7_release/physical_report.py reports/local/m7-release.log \
  --json reports/local/m7-release.json \
  --markdown reports/local/m7-release.md
```

Automated physical PASS requires, among other gates:

- at least 131072 measured blocks;
- `HARD_GATE failure_flags=0`;
- zero active synth queue overflow, producer stall, write timeout and partial write;
- no clipping;
- stack headroom gate PASS;
- `backlog_exceeded_dma_window=0`;
- `final_backlog_us=0` and `backlog_healthy=1`;
- normal/full-FX maximum >= 24 and heavy-DSP maximum >= 8;
- music-stress PASS, I2S PASS and zero leaked final Voice refs.

The report parser has deterministic host tests for clean PASS, hard-gate failure, queue overflow, unrecovered/DMA-exceeding backlog, insufficient measured duration, missing telemetry and panic markers.

## Audible evidence boundary

Subjective audible evidence stays separate from automated timing evidence.

The current Korvo/ES8311 qualification output is mono: renderer L/R is averaged before the single DAC/speaker path. Therefore this board can physically audition pitch, loop/envelope behavior, drums and FX, and can prove that stereo-derived content survives mono downmix, but it cannot prove a spatial stereo image or left/right pan by listening to its single speaker.

For M7 closure:

- use the existing real-SF2 dry cue for pitch/envelope sanity;
- add/document representative sustained/loop, drum and FX listening samples;
- use host/stereo PCM evidence for left/right and pan semantics, with physical mono-downmix as the N16R8 hardware check;
- do not describe the mono Korvo listening run as a true stereo audition.

## Remaining M7 hardware work

1. Flash the `esp32s3-n16r8-m7-release-longrun` artifact from checkpoint `22b48b2f`.
2. Capture one complete boot/qualification serial log through the end of the 131072-block run.
3. Run `physical_report.py` and retain its JSON/Markdown evidence.
4. Record the representative audible observations separately.
5. Only then mark M7 complete and freeze product-facing compatibility/polyphony wording.
