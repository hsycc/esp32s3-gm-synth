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

The fast runner preserves the existing 23-step Phase G build/test matrix but
omits only the full-FM calls embedded in the preparer and Program Change test
entry points. The 6 MiB checked-in fixtures remain the normal semantic/regression
baseline.

## Full-bank GM qualification

`.github/workflows/gm128-full-bank.yml` runs:

```bash
bash scripts/run_gm128_full_bank_tests.sh
```

The dedicated lane has four gates:

1. `test_full_gm128_qualification` scans the actual full bank and runs every
   tracked preset through `PlaybackPreparePolicy::StrictQualified`.
2. The canonical `test_program_change_control_service` integration binary
   exercises the production residency/publication/READY/commit/ACK/NoteOn path,
   including full-FM Program Change, channel-10 percussion and 16-channel GM
   Reset.
3. `test_full_gm128_stress` exercises product-sized 4 MiB residency pressure
   and the full-bank functional stress baseline. It sweeps all 128 bank-0
   melodic programs and all 128 bank-128 percussion programs through real
   Program Change transactions, verifies safe cache eviction, exercises real
   Bank Select and latest-wins Program Change coalescing, then runs a
   representative 15-melodic-plus-drum 16-channel voice workload with dense
   piano, sustain/release, repeated retrigger, drum bursts and live controller
   changes.
4. `test_full_gm128_capacity` derives the representative 16-channel Active
   dependency union, scans every bank-0 melodic READY candidate while the old
   Active set remains pinned, identifies the largest Active+READY union, then
   runs the real residency/publication/READY/commit/ACK transaction against
   4.00/4.25/4.50/4.75/5.00/5.50/6.00 MiB arenas. This produces a host capacity
   bracket without prematurely changing the N16R8 product default.

The qualification report is uploaded as the
`gm128-full-bank-qualification` Actions artifact and the `GM128_*` lines are
also copied to the workflow summary.

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

The first product-sized stress run established:

- bank-0 programs 0-127 complete 127 sequential Program Change transactions in
  a 4 MiB arena;
- the melodic sweep reaches 4,184,380 bytes high-water and performs 92 safe
  cache evictions / capacity retries;
- bank-128 programs 0-127 complete the same sweep with a 2,969,748-byte
  high-water and no eviction;
- real Bank Select reaches a full-bank variation bank and a burst of 128
  Program Changes coalesces to one latest-wins request;
- the representative 15-melodic-plus-drum 16-channel Active set consumes
  4,310,396 aligned PCM bytes, which is 116,092 bytes above the current 4 MiB
  arena default;
- the corresponding functional workload reaches 16 active Voices with zero
  voice-start failures and zero sample acquire misses.

Therefore 4 MiB is already proven for complete single-channel GM128 switching,
but it is not large enough for the representative 16-channel Active set. The
capacity-curve gate determines the next useful host bound; the production arena
default must still wait for N16R8 PSRAM headroom measurements.

## Reported metrics

The structure report records bank topology, resolved/playback zone bounds,
sample dependency counts, per-program PCM working sets, Prepare workspace,
modulator identities and preview/unsupported counts.

The stress/capacity reports add:

- 4 MiB melodic/drum Program Change transition counts and high-water marks,
- safe cache eviction/retry counts and evicted bytes,
- full-bank Bank Select and rapid Program Change coalescing coverage,
- representative 16-channel resident/high-water bytes,
- peak active Voice count, starts/failures and sample-acquire misses,
- representative Active dependency union,
- worst bank-0 Active+READY dependency union and candidate identity,
- pass/fail/commit result for each tested sample-arena capacity,
- first tested arena size that completes the real Active+READY transaction.

These host metrics establish semantic, residency and bounded voice-behavior
coverage. They do **not** replace N16R8 measurements for storage latency, total
PSRAM headroom outside the sample arena, DSP CPU, 64-frame I2S deadline misses,
underruns or stable guaranteed polyphony.
