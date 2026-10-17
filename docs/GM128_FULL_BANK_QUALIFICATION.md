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
entry points. Their source files are restored before the runner exits. The 6 MiB
checked-in fixtures remain the normal semantic/regression baseline.

## Full-bank GM qualification

`.github/workflows/gm128-full-bank.yml` runs:

```bash
bash scripts/run_gm128_full_bank_tests.sh
```

The lane has two gates:

1. `test_full_gm128_qualification` scans the actual full bank and runs every
   tracked preset through `PlaybackPreparePolicy::StrictQualified`.
2. The canonical `test_program_change_control_service` integration binary
   exercises the production residency/publication/READY/commit/ACK/NoteOn path,
   including full-FM Program Change, channel-10 percussion and 16-channel GM
   Reset.

The qualification report is uploaded as the
`gm128-full-bank-qualification` Actions artifact and the `GM128_*` lines are
also copied to the workflow summary.

## Reported metrics

The report records the bank topology and the runtime-facing resource bounds:

- total presets and exact bank/program coverage,
- melodic and percussion preset counts,
- sample count and `smpl` payload bytes,
- total and maximum native resolved zones,
- total and maximum final playback zones after stereo expansion,
- stereo-pair count,
- total, maximum and unique sample dependencies,
- largest per-program logical/aligned PCM dependency working set,
- maximum combined Prepare scratch/publication storage requirement,
- supported/unsupported modulator record counts,
- non-zero `pmod` / `imod` source-destination identity tuples,
- preview-zone count.

The gate additionally requires all bank-0 programs 0-127, the standard
bank-128/program-0 drum kit, representative key/velocity coverage, GM drum keys
35-81, zero unsupported modulators and zero preview zones.

These host metrics are semantic/resource qualification data only. They do not
replace N16R8 measurements for storage latency, PSRAM pressure, DSP CPU, I2S
deadline misses, underruns or stable polyphony.
