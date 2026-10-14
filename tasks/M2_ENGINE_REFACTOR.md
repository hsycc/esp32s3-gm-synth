# M2 — Engine Refactor

## Goal
Decompose the initial SF2 reference-derived implementation into clear realtime domains without intentionally changing sound behavior.

## Target modules
- `gm/` — channel semantics
- `sf2/` — parsing and zone resolution
- `sample/` — sample handles/residency
- `synth/` — voices/allocation
- `dsp/` — interpolation/envelopes/filter/FX
- `audio/` — block render/I2S
- `diagnostics/` — counters/timing

## Rules
- add characterization/unit tests before moving behavior
- preserve audio behavior during structural moves
- separate parsing from PCM residency/loading
- keep sample-cache ownership out of generic MIDI channel state
- avoid a new God object replacing the old one

## Exit criteria
The realtime render path and control/asset path are visibly separated, build/tests remain green, and no intentional DSP behavior change is mixed into the refactor.