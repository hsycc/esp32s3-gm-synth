# M1 — SF2 Analyzer

## Goal
Analyze the actual fixed GM bank and turn compatibility/memory questions into data.

## Tool
Create a host-side analyzer under `tools/sf2_analyzer/`.

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
Architecture decisions about residency and SF2 feature support are based on analyzer output, not `.sf2` file size guesses.