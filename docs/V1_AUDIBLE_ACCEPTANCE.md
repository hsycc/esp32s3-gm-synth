# V1 audible acceptance worksheet

Status: **NOT RUN**. A serial PASS, a matching PCM checksum or generated WAV files
do not complete any listening row. Fill observations only after actual listening.
This worksheet does not implement new firmware, MIDI ingress or a new player.

## Record the exact audition setup

- Date / listener: **NOT RECORDED**
- Source commit and full local ELF/bin SHA-256: **NOT RECORDED**
- Device/module and output path (speaker/headphone/stereo interface): **NOT RECORDED**
- SoundFont path + SHA-256; Bank / Program / sample identity: **NOT RECORDED**
- MIDI/test clip, note/velocity/controller sequence: **NOT RECORDED**
- Synth master, DAC attenuation, PA/speaker/load setting: **NOT RECORDED**
- Recording file + SHA-256, or timestamped observation without recording: **NOT RECORDED**

Start with master 32767 / DAC -12 dB, the tested baseline. Alternative loudness
settings need a separately identified image/config after preserving source
provenance. Listen at a comfortable level. Digital `clipped_samples=0` does not
establish analog headroom. Do not mark physical stereo/pan PASS using a single
ES8311 speaker: this board's qualification path averages L/R to mono.

## Case matrix

| ID | Case and observation | Currently available basis | Result |
|---|---|---|---|
| A1 | Clean SF2 attack, C4 sustain, C-major chord, note release; note audible clicks, wrong pitch, unexpected retrigger or abrupt truncation | Existing frozen image logs advertise `C4_2s,C_major_3s,C4_E4_G4_C5`, `real_sf2_dry`; this is one prepared program, NOT every piano/string preset | NOT RUN |
| A2 | Reference tone, start/stop and PA transition; identify where a pop occurs | Existing 441 Hz reference-tone / clean / stress boot sequence; distinguish test-tone transitions from musical artifacts | NOT RUN |
| A3 | CC21 sweep while voices sound; filter change should be audible without a new unintended note or unrelated gain/pitch jump | Existing synthetic physical stress fixture, logged `0,64,127,0`; not a real-bank CC21 audition | NOT RUN |
| A4 | Representative real-bank long-loop / string or pad, drums and release tails | Requires an explicitly selected preset/clip and an actual audition path; do not assume the current benchmark provides these as real-bank audio | NOT RUN |
| A5 | Reverb/chorus send changes, tail continuity and mute behavior at intended release volume | Need actual audible/recorded evidence; stress counters alone are insufficient; current logs cover unity master only | NOT RUN |
| A6 | Loop A/B sample 604: direct vs 16f/32f/64f; click, pitch/timbre and repetition | Existing GeneralUser host WAV exporter / previously supplied WAVs | NOT RUN |
| A7 | Loop A/B sample 602: direct vs 16f/32f/64f; same checks | Existing GeneralUser host WAV exporter / previously supplied WAVs | NOT RUN |
| A8 | Loop A/B sample 62: direct vs 16f/32f/64f; prefer no processing when it is already clean | Existing GeneralUser host WAV exporter / previously supplied WAVs | NOT RUN |
| A9 | Left/right and pan semantics | Host stereo PCM evidence or a real stereo output path; mono speaker audition is NOT APPLICABLE for spatial correctness | NOT RUN |

The qualification image is an automated benchmark, not an established manual
MIDI instrument frontend. Do not request arbitrary live notes/program selection
without first providing and validating such an input path. A4/A5 require that
additional audition work or an existing verified host rendering path.

## Reuse the existing loop A/B exporter

Prefer the already supplied twelve WAVs, preserving their report and input asset
identity. After source evidence has been preserved, the repository's existing
command (also used by `.github/workflows/m7-loop-seam-audio.yml`) is:

```sh
python3 tools/m7_loop_seam_audio.py assets/sf2/full/GeneralUser-GS.sf2 \
  --output build/m7-loop-seam-audio --seconds 2.0
```

This produces original / 16 / 32 / 64-frame variants for the three samples. It
is host sample-loop preparation, not a full SF2 performance rendered through the
ESP32 DAC. Keep playback volume identical when comparing. Do not regenerate
these by rebuilding or reflashing the frozen hardware image.

## Per-case observation (copy this block)

Case ID / exact file or clip: **NOT RECORDED**

Output path / settings / timestamp: **NOT RECORDED**

What was actually heard (include where a problem occurs): **NOT RECORDED**

Decision: **NOT RUN** (replace with PASS, FAIL or DEFERRED + explicit reason).

Evidence reference: **NOT RECORDED**

For 604/602/62: selected variant **UNDECIDED**; audible advantage **UNRECORDED**;
period/pitch/timbre trade-off **UNREVIEWED**. No global runtime crossfade approved.

## Exit rule

A reviewer must record supported observations and a disposition for each release
requirement. Missing listening stays missing; an explicit scope deferral is not
PASS. New audible bugs requiring DSP changes reopen the relevant automated and
physical gates. The current closeout tools themselves change no audio behavior.
