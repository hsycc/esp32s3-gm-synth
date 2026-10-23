# GM128 Full-Bank Qualification

The tracked FM qualification bank is:

`assets/sf2/full/FM_GM_SoundFont_v0_2_1_mini.sf2`

It is validated in the dedicated `gm128-full-bank` lane so the normal Phase G semantic loop does not depend on repeated 14+ MB full-bank scans.

## Dedicated host gates

`bash scripts/run_gm128_full_bank_tests.sh` contains six release-oriented gates:

1. full-bank structure / StrictQualified scan;
2. production-shaped Program Change / GM Reset runtime-control transactions;
3. 4 MiB single-channel Program Change/cache pressure plus mixed functional stress;
4. representative 16-channel Active+READY capacity curve;
5. discovery-driven worst-program/release-tail checks plus a deterministic 32-second mixed host soak;
6. per-program M7 release matrix for Bank 0 melodic 0–127 and Bank 128 percussion 0–127.

The first five gates preserve the established GM128 host semantic/residency/lifetime baseline. The sixth adds a stable per-Program release-reporting surface rather than replacing the existing tests.

## Current tracked-bank findings

The full FM asset contains:

- `5,760` presets across `45` banks;
- `128` programs in every discovered bank;
- `175` samples;
- `14,056,220` bytes of `smpl` payload;
- `5,760 / 5,760` StrictQualified presets;
- zero prepare, metadata, dependency or duplicate-identity failures;
- zero Preview zones and zero unsupported modulator records.

The asset itself contains no non-zero `pmod` / `imod` records and no stereo pairs, so separate compatibility fixtures and representative user banks remain required for those features.

Bank `128`, program `0` is the largest prepared program in the existing aggregate scan at `47` playback zones, `47` dependencies and `2,969,748` aligned PCM bytes. Discovery across Bank 0 + Bank 128 confirms the maximum playable NoteOn layer count is only `1`; the 47 drum zones are a key map, not simultaneous layers.

## Product-sized residency findings

The 4 MiB melodic 0–127 Program Change sweep passes and reaches `4,184,380` bytes high-water while exercising `92` safe cache evictions. The Bank-128 0–127 sweep passes at `2,969,748` bytes high-water.

The representative 15-melodic-plus-drum Active set requires `4,310,396` aligned PCM bytes across `62` unique samples. Scanning every Bank-0 READY candidate finds the largest union at channel `0`, program `122`: `4,504,436` bytes.

Real transaction trials establish:

- 4.00 MiB: representative Active set does not fit;
- 4.25 MiB: Active fits, worst READY does not;
- 4.50 MiB: Prepare + READY + block-boundary commit + ACK pass;
- 4.75/5.0/5.5/6.0 MiB: pass.

The completed M6 physical run also demonstrated that the 4.50 MiB qualification arena coexists with the complete N16R8 qualification runtime. This does not automatically redefine the normal firmware default arena policy.

## Release-tail and long-soak findings

The discovery-driven gate finds the longest volume-envelope release in the scanned bank at `26,465` frames. The bounded runtime case uses Bank 0 / Program `88` / key `0` / velocity `1`, warms the envelope for `151` frames, starts `8` overlapping instances, and naturally releases all of them within `26,496` rendered frames. Final published Voice refs are `0` with zero stale-release faults.

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

## M6 physical closure

M6 physical product qualification is complete at:

`c16ff278b265018be9d2af5cc4fe3e54fe01e947`

The committed baseline is:

`benchmarks/M6_PHYSICAL_QUALIFICATION_BASELINE_2026-10-20.md`

Physical conclusions:

- normal/full-FX stable through **24 Voices**;
- 28 Voices is the first tested sustained unstable step;
- pathological heavy-DSP stable through **8 Voices**;
- 12 Voices is the first heavy-DSP unstable step;
- final 16384-block mixed music stress: hard-gate flags `0`, max backlog `448 us`, final backlog `0`, minimum DMA margin `11162 us`;
- active playback queue overflows, write timeouts, partial writes and clipped samples: `0`.

The 1451 us 64-frame period remains a soft transient timing diagnostic in the production-shaped mixed timeline. Sustained polyphony boundary sweeps remain strict realtime tests.

## M7.0 per-program release matrix

M7 begins by making the already-qualified GM128 evidence easier to consume and extend.

`tests/gm/test_gm128_release_matrix.cpp` emits one `M7_PROGRAM` row for every:

- Bank 0 melodic program 0–127;
- Bank 128 percussion program 0–127.

Rows expose the production-preparer result plus zones, dependencies, PCM working set, modulator state and key/drum coverage. `tools/gm128_release/report.py` parses these rows and produces:

- `m7-release-program-matrix.json` — machine-readable `gm128-release-matrix-v1` schema;
- `m7-release-program-matrix.md` — release summary with coverage, maxima and the largest PCM programs.

The `gm128-full-bank` workflow uploads both files with the raw qualification log. This schema is the seed for M7.1 multi-bank native-SF2 qualification.

## Next release boundary

The tracked compact bank is no longer the only release target. M7 proceeds with:

- the same per-Program matrix across representative public/user native `.sf2` banks;
- explicit classification of stereo/modulator/complex-zone features absent from the compact tracked bank;
- real-bank mixed 16-channel workload qualification;
- malformed/unsupported SF2 rejection behavior;
- physical long-run and representative audible release sampling.

See `tasks/M7_GM128_USER_SF2_RELEASE.md`.
