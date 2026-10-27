# V1 RC1 — physical audition execution plan

Source baseline: `main` at `596b234c9a8fe5c93a3e46ecd42798579ef7b90c`.
Status: **READY TO EXECUTE / AUDIBLE EVIDENCE NOT YET RECORDED**.

## What is already qualified

M8 3/3 long runs (frozen image `bb31908b`) passed DMA hard gates and retained identical PCM checksum `fe69915701738934`, with 24 normal/full-FX sample voices and 8 pathological heavy-DSP voices stable. The tested physical path sends a mono average to ES8311; it cannot qualify left/right speaker positioning. The historical `-dirty` suffix was expressly accepted by the user for integration and is retained in the records.

The `main` merge does not create new listening evidence. Keep the default Post-FX master=32767 and DAC attenuation=12 dB for baseline observations. This plan does **not** change the audio engine or assert that an audible quality check has passed.

## Sequence A — audition the existing physical fixture (no new feature required)

Use a clean checkout of the specified baseline or an unchanged, explicitly identified prebuilt image with matching settings. Keep the actual app banner and ELF hash in the record. The existing qualification starts a 441 Hz reference tone, then a real-SF2 C4 and major chord cue, then a synthetic musical stress timeline containing the CC21 0→64→127→0 sweep.

1. Record output hardware (Korvo V4 / ES8311 / PA / speaker), supply, volume, listening position and source revision. Start at a conservative acoustic level.
2. Listen to reference tone at boot and after enabling the PA. Mark unwanted clicks, pop or distortion separately from intended tone start/stop.
3. Listen to the real-SF2 clean cue (C4 hold and C-major chord), including note release. Note pitch, attack, sustain, clicks, abrupt cut-off or unexpected notes. This is **one program**, not a general proof of piano/string quality.
4. Listen for the CC21 sweep during the subsequent stress fixture; capture the timestamp and describe any unintended pitch or gain change. This synthetic timing fixture is not necessarily an ideal perceptual demonstration of filter sweeps; if inaudible, record **INCONCLUSIVE**, not PASS.
5. Preserve the complete monitor log, note each case result in `docs/V1_AUDIBLE_ACCEPTANCE.md` or a separate dated worksheet. Do not mark cases without an actual listener as PASS.

Do not re-run the full six-minute test solely to reproduce the already-qualified DMA performance unless needed to capture the acoustic signal.

## Sequence B — compare existing host WAV loop-seam variants

Use the previously generated GeneralUser sample 604 / 602 / 62 WAV sets (direct, 16f, 32f, 64f). The repository's exporter can regenerate them separately from the firmware image:

```sh
python3 tools/m7_loop_seam_audio.py assets/sf2/full/GeneralUser-GS.sf2 \
  --output build/m7-loop-seam-audio --seconds 2.0
```

For each sample, keep playback level fixed and compare transient click, periodicity, pitch and timbre. Mark a chosen variant only after listening. These WAVs are host sample-loop renderings **not** ES8311 captured output, and they do not authorize global DSP crossfade.

## Sequence C — requires a dedicated, verified audition route

A4 representative GeneralUser/FluidR3 piano/string/pad/drum presets, A5 reverb/chorus control and stereo-pan checks require either a documented MIDI/program-selection input with on-device output or deterministic host stereo PCM rendering. The current automatic qualification image is not a verified manual GM player. Do not issue arbitrary live MIDI/program commands or claim physical stereo qualification through mono ES8311. Identify and validate the audition route before collecting A4/A5/A9 results.

## Sequence D — DAC loudness/headroom, deliberately isolated

M8 speed/PCM evidence stays pinned to master Q15=32767, DAC attenuation=12 dB (-12 dB). A subsequent independent build may set `CONFIG_GM_SYNTH_I2S_QUAL_DAC_ATTENUATION_DB=6` (-6 dB) using a dedicated SDKCONFIG defaults override. Do not edit the frozen build, simply overwrite its sdkconfig, or compare the louder image as the same performance binary. Record full source/config/artifact IDs, codec readback, load/speaker, listening results, and where practical scope/record the analog output to test actual clipping. PCM `clipped_samples=0` alone cannot prove analog headroom.

## Minimum closeout evidence per case

| Field | Required evidence |
|---|---|
| Test case | A1–A9 from `docs/V1_AUDIBLE_ACCEPTANCE.md` |
| Image identity | Source SHA, app banner, full ELF SHA256 if available |
| Asset/trigger | SF2 hash/Bank/Program or named sample WAV; event timeline |
| Playback chain | ES8311, PA, speaker/load or stereo recording output |
| Result | PASS / FAIL / INCONCLUSIVE / NOT RUN / DEFERRED with reason |
| Observation | Exact audible symptom, location/timestamp, optional recorded WAV |

Missing listening evidence remains NOT RUN. A problem requiring a DSP change must be isolated and requalified separately. No V1 binary should be released based solely on CI and synthetic stress.
