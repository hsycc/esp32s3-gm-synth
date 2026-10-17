# GM128 Full-Bank Qualification

The tracked FM qualification bank is:

`assets/sf2/full/FM_GM_SoundFont_v0_2_1_mini.sf2`

It is validated in the dedicated `gm128-full-bank` host lane so the normal Phase G semantic loop does not depend on repeated 14+ MB full-bank scans.

## Fast Phase G

`.github/workflows/m6-sf2-playback-prepare-tests.yml` runs the fast consolidated host matrix. The full FM qualification remains isolated in `.github/workflows/gm128-full-bank.yml`.

## Dedicated full-bank gates

`bash scripts/run_gm128_full_bank_tests.sh` now contains five gates:

1. full-bank structure / StrictQualified scan;
2. production-shaped Program Change / GM Reset runtime-control transactions;
3. 4 MiB single-channel Program Change/cache pressure plus mixed functional stress;
4. representative 16-channel Active+READY capacity curve;
5. discovery-driven worst-program/release-tail checks plus a deterministic 32-second mixed host soak.

The workflow uploads `gm128-full-bank-qualification` and copies `GM128_*` metrics to the Actions summary.

## Current tracked-bank findings

The full FM asset contains:

- `5,760` presets across `45` banks;
- `128` programs in every discovered bank;
- `175` samples;
- `14,056,220` bytes of `smpl` payload;
- `5,760 / 5,760` StrictQualified presets;
- zero prepare, metadata, dependency or duplicate-identity failures;
- zero Preview zones and zero unsupported modulator records.

The asset itself contains no non-zero `pmod` / `imod` records and no stereo pairs, so separate compatibility fixtures remain required for those features.

Bank `128`, program `0` is the largest prepared program at `47` playback zones, `47` dependencies and `2,969,748` aligned PCM bytes. Discovery across bank 0 + bank 128 confirms the maximum playable NoteOn layer count is only `1`; the 47 drum zones are a key map, not simultaneous layers.

## Product-sized residency findings

The 4 MiB melodic 0–127 Program Change sweep passes and reaches `4,184,380` bytes high-water while exercising `92` safe cache evictions. The bank-128 0–127 sweep passes at `2,969,748` bytes high-water.

The representative 15-melodic-plus-drum Active set requires `4,310,396` aligned PCM bytes across `62` unique samples. Scanning every bank-0 READY candidate finds the largest union at channel `0`, program `122`: `4,504,436` bytes.

Real transaction trials establish:

- 4.00 MiB: representative Active set does not fit;
- 4.25 MiB: Active fits, worst READY does not;
- 4.50 MiB: Prepare + READY + block-boundary commit + ACK pass;
- 4.75/5.0/5.5/6.0 MiB: pass.

Therefore 4.5 MiB is the current host lower bound for this scanned 16-channel Active+READY model. It is **not** yet the production default; physical N16R8 total-PSRAM headroom remains authoritative.

## Release-tail and long-soak findings

The discovery-driven gate finds the longest volume-envelope release in the scanned bank at `26,465` frames. The bounded runtime case uses bank 0 / program `88` / key `0` / velocity `1`, warms the envelope for `151` frames, starts `8` overlapping instances, and naturally releases all of them within `26,496` rendered frames. Final published Voice refs are `0` with zero stale-release faults.

The mixed soak renders `1,411,200` frames (`32,000 ms` of 44.1 kHz audio-domain time) in 64-frame blocks using a 4.5 MiB host sample arena. It produces:

- sample-arena high-water: `4,310,396` bytes;
- peak active Voices: `16`;
- Voice starts: `2,080`;
- Voices finished: `2,080`;
- rendered Voice frames: `17,141,721`;
- published-sample acquire misses: `0`;
- final live Voice refs: `0`;
- deterministic checksum: `11224005462836535547`.

This is semantic/lifetime/boundedness evidence, not a host wall-clock performance claim.

## Qualification boundary

Host GM128 semantic/residency/lifetime qualification is now substantially complete. The remaining release decision is physical N16R8 qualification for:

- candidate 4.5 MiB arena versus total PSRAM/SRAM headroom;
- cold/warm/eviction Program Prepare timing;
- 64-frame render p95/p99/max timing;
- 16/18/20 Voice load matrix where reachable;
- dry/reverb/chorus/filter paths;
- deadline misses and I2S underruns;
- event queue drops and invalid sample/fetch accesses;
- Voice stealing behavior;
- long-running mixed playback stability;
- final guaranteed polyphony.

The committed host baseline is `benchmarks/GM128_HOST_QUALIFICATION_BASELINE_2026-10-18.md`.
