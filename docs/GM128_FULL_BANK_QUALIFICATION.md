# GM128 Full-Bank Qualification

The tracked FM qualification bank is:

`assets/sf2/full/FM_GM_SoundFont_v0_2_1_mini.sf2`

It is intentionally validated in a dedicated host lane instead of making the
normal Phase G semantic loop depend on a 14+ MB bank scan.

## Fast Phase G

`.github/workflows/m6-sf2-playback-prepare-tests.yml` runs:

```bash
bash scripts/run_phase_g_fast_host_tests.sh
```

The fast runner preserves the existing Phase G build/test matrix while the full
14+ MB bank work stays in the dedicated GM128 lane.

## Full-bank GM qualification

`.github/workflows/gm128-full-bank.yml` runs:

```bash
bash scripts/run_gm128_full_bank_tests.sh
```

The dedicated lane has five gates:

1. `test_full_gm128_qualification` scans the actual full bank and runs every
   tracked preset through `PlaybackPreparePolicy::StrictQualified`.
2. The canonical `test_program_change_control_service` integration binary
   exercises residency/publication/READY/commit/ACK/NoteOn, full-FM Program
   Change, channel-10 percussion and 16-channel GM Reset.
3. `test_full_gm128_stress` exercises the product-sized 4 MiB single-channel
   residency path, safe cache eviction, Bank Select, latest-wins Program Change,
   dense piano, sustain/release, retrigger, drum bursts and mixed 16-channel
   functional behavior.
4. `test_full_gm128_capacity` derives the representative 16-channel Active
   dependency union, scans every bank-0 melodic READY candidate while Active is
   pinned, then runs the real transaction across 4.00/4.25/4.50/4.75/5.00/5.50/
   6.00 MiB arenas.
5. `test_full_gm128_soak` is discovery-driven. It scans bank 0 and bank 128 for
   the largest playable NoteOn layer count, the largest prepared zone set and a
   bounded long-release candidate, exercises those exact programs through the
   production Voice path, verifies natural release-tail reference reclamation,
   then renders 1,411,200 frames (32.0 seconds of 44.1 kHz audio time) of mixed
   16-channel event churn using a 4.5 MiB host sample arena.

The qualification report is uploaded as the
`gm128-full-bank-qualification` Actions artifact and `GM128_*` lines are copied
to the workflow summary.

## Current tracked-bank findings

The full FM asset contains 5760 presets across 45 banks, with 128 programs in
each bank. All 5760 presets pass StrictQualified preparation with zero preview
zones and zero unsupported modulator records. The file contains 175 samples;
this asset itself has no non-zero pmod/imod records and no stereo pairs, so the
separate GeneralUser/FluidR3 compatibility fixtures remain necessary for those
features.

The heaviest single prepared program is bank 128 / program 0 at 47 playback
zones and 47 sample dependencies. Its aligned PCM working set is 2,969,748
bytes.

The product-sized stress/capacity evidence establishes:

- bank-0 programs 0-127 complete sequential Program Change transactions in a
  4 MiB arena;
- the melodic sweep reaches 4,184,380 bytes high-water and performs 92 safe
  cache evictions/capacity retries;
- bank-128 programs 0-127 complete the same sweep at 2,969,748 bytes high-water;
- the representative 15-melodic-plus-drum 16-channel Active set consumes
  4,310,396 aligned PCM bytes (62 unique samples);
- the worst scanned READY candidate is channel 0 / program 122 and raises the
  Active+READY union to 4,504,436 bytes;
- 4.25 MiB can hold Active but cannot prepare that READY generation;
- 4.50 MiB is the first tested arena size that completes Prepare, READY,
  block-boundary commit and ACK; 4.75/5.0/5.5/6.0 MiB also pass.

Therefore 4 MiB remains proven for complete single-channel GM128 switching but
is not sufficient for the representative 16-channel Active set. 4.5 MiB is the
current host lower bound for the scanned worst Active+READY transition, not yet
a production default. N16R8 hardware still has to prove total PSRAM headroom
with Voice/DSP/FX/audio buffers and system allocations present.

## Reported metrics

The full-bank artifact reports topology, zone/dependency/PCM bounds, 4 MiB
Program Change and eviction metrics, 16-channel residency, Active+READY capacity
trials, max-layer/max-zone discovery, release-tail reclamation and the 32-second
host-soak counters/checksum.

These host metrics establish semantic, residency and bounded lifetime behavior.
They do **not** replace N16R8 measurements for storage latency, total PSRAM
headroom outside the sample arena, DSP CPU, 64-frame I2S deadline misses,
underruns or stable guaranteed polyphony.
