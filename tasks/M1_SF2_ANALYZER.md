# M1 — SF2 Analyzer

## Goal
Analyze real candidate GM banks and turn compatibility/memory questions into data for the fixed ESP32-S3 N16R8 target.

## Tool
Host-side analyzer under `tools/sf2_analyzer/`.

## Implementation status

Analyzer v0.1 is implemented with Python standard library only.

Implemented:
- RIFF/SF2 chunk parsing
- `phdr/pbag/pmod/pgen/inst/ibag/imod/igen/shdr` table parsing
- preset -> instrument -> sample dependency resolution
- preset/instrument key and velocity range intersection
- sample PCM byte and 4-byte-aligned residency accounting
- generator usage reporting
- modulator record reporting
- linked/stereo sample reporting
- per-program sample working sets and representative layer overlap
- conventional bank-0 / bank-128 GM summaries
- greedy 16-program working-set estimate
- greedy 15-melodic + 1-drum working-set estimate
- optional PSRAM/reserve fit calculation
- JSON output
- synthetic SF2 host tests and GitHub Actions coverage

## Real candidate analysis

Two user-supplied banks were analyzed locally. Source `.sf2` binaries are not committed.

### TimGM6mb.sf2
- source size: ~5.72 MiB
- `smpl` payload: ~5.52 MiB
- aligned PCM sample sum: ~5.47 MiB
- 136 presets = 128 bank-0 melodic + 8 bank-128 drum presets
- 210 instruments
- 520 samples
- all samples are mono
- substantial `imod` usage: 455 non-terminal records, 193 with non-zero amount
- 15 melodic + worst drum 16-channel union observed: ~4.02 MiB
- complete source PCM may fit R8, but only if real firmware/FX/metadata reserve proves sufficient

### GMBK5X128.sf2
- source size: ~15.06 MiB
- `smpl` payload: ~14.92 MiB
- aligned PCM sample sum: ~14.84 MiB
- 407 presets across many banks
- bank 0 contains 128 melodic programs
- bank 128 contains 10 drum presets
- 281 instruments
- 887 samples
- all samples are mono and 48 kHz
- no effective source `pmod/imod` records observed
- bank-0 sample union: ~7.24 MiB
- bank-0 + standard drum sample union: ~8.49 MiB
- 15 melodic + standard drum 16-channel union observed: ~5.44 MiB
- worst tested 15 melodic + drum union across its drum presets: ~5.80 MiB
- full residency is impossible on R8
- raw 15 MiB source should not be the deployed N16 asset; offline GM-only pruning/compiler stage is required if this bank is selected

See `docs/SF2_BANK_ANALYSIS.md` for the comparison and architectural consequences.

## Architecture decisions now supported by M1 data

1. N16R8 is the fixed memory target; R16 is excluded.
2. SF2 source file size is not a product target.
3. Shared/per-program residency is a required V1 capability.
4. Full residency remains an optional optimization for compact compiled banks such as TimGM6mb if hardware reserve allows it.
5. A host-side bank compiler/pruner is required for large multi-bank sources such as GMBK5X128.
6. Runtime SF2 compatibility should be scoped to observed bank semantics; TimGM6mb and GMBK5X128 stress different compatibility areas.

## Still required to complete M1

- extend report output with sample-rate/bank distributions and clearer modulator classification
- generate normalized machine-readable reports for both candidate files
- audit the used generators/modulators against the planned firmware implementation
- validate suspicious/unsupported semantics against the SF2 specification/reference synth behavior
- establish a measured N16R8 PSRAM reserve target on hardware

## Exit criteria
Architecture decisions about residency and SF2 feature support are based on analyzer output from real candidate banks and N16R8 measurements, not `.sf2` file size guesses.
