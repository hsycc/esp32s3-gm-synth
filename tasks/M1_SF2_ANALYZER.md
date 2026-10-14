# M1 — SF2 Analyzer

## Goal
Analyze the actual fixed GM bank and turn compatibility/memory questions into data.

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
- non-zero `pmod` / `imod` reporting
- linked/stereo sample reporting
- per-program sample working sets and representative layer overlap
- conventional bank-0 / bank-128 GM summaries
- greedy 16-program working-set estimate
- greedy 15-melodic + 1-drum working-set estimate
- optional PSRAM/reserve fit calculation
- JSON output
- synthetic SF2 host tests and GitHub Actions coverage

Still required to complete M1:
- run the analyzer against the actual candidate production GM bank
- archive the generated JSON report (when licensing/confidentiality permits)
- validate suspicious/unsupported generator and modulator usage against the SoundFont spec and reference synth behavior
- use the real report to choose full-bank vs program-resident sample strategy

## Required output
- RIFF/SF2 chunk summary
- file size and `smpl` PCM payload size
- preset/instrument/sample counts
- aligned PCM residency estimate
- used generator opcodes
- presence/usage of `pmod` / `imod`
- linked/stereo sample usage
- per-program zone count
- per-program unique sample set
- per-program PCM working-set size
- representative max overlapping zones per note/velocity
- drum-bank requirements
- JSON report suitable for CI/tests

## Questions this milestone must answer
1. Can the whole fixed bank fit in target PSRAM with safe reserve?
2. Which SF2 features are actually required?
3. Which programs have the largest working sets/layering?
4. What are realistic 16-channel union working sets?

## Exit criteria
Architecture decisions about residency and SF2 feature support are based on analyzer output from the actual fixed GM bank, not `.sf2` file size guesses.
