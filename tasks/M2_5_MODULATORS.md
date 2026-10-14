# M2.5 — SF2 Modulator Resolver（调制器解析器）

## Goal

Support mainstream SoundFont dynamic-control semantics without interpreting arbitrary `pmod` / `imod` tables inside the per-sample audio hot loop.

## Terminology

- Default Modulator（默认调制器）: SoundFont-defined controller routes that exist even if the file contains no custom modulator records.
- `imod` / Instrument Modulator（乐器级调制器）: instrument-zone rules which may overwrite identical defaults.
- `pmod` / Preset Modulator（预置级调制器）: preset-zone rules which add to instrument/default modulation.
- ModPlan（调制计划）: pre-resolved event/control-rate representation consumed by runtime voices.

## Phase A — identity and merge semantics

Implemented/started:
- decode `sfModulator` source bit fields
- standard/default modulator definitions
- global/local identity merge
- Instrument `OVERWRITE` semantics
- Preset `ADD` semantics
- preserve zero-amount `imod` so it can disable a default modulator
- ignore zero-amount `pmod` as an additive no-op
- group effective modulators by their update trigger:
  - NoteOn/static
  - MIDI CC
  - Pitch wheel/sensitivity
  - Channel pressure
  - Poly pressure

Host tests must lock these semantics before DSP integration.

## Phase B — source mapping/evaluation

Implement and cross-check:
- Linear（线性）
- Concave（凹曲线）
- Convex（凸曲线）
- Switch（开关）
- Positive/Negative（正向/反向）
- Unipolar/Bipolar（单极/双极）
- primary source × amount source
- supported transform operator behavior

Reference selected cases against FluidSynth/SoundFont behavior.

## Phase B.5 — pinned real-bank oracle

The conformance oracle is pinned to FluidSynth 2.6.1 commit
`71e85b2ca6bf48641ba7e2261d8f2640ae473773`.

The probe is compiled inside that exact source tree and intentionally calls the
internal `fluid_voice_gen_value()` accessor. Do **not** replace it with the
public `fluid_voice_gen_get()` API for effective-value comparisons:

- `fluid_voice_gen_get()` exposes the nominal/base generator value.
- `fluid_voice_gen_value()` exposes the synthesis-effective
  `val + mod + nrpn` value.

Using the public base-value getter as an effective oracle creates false
mismatches for default velocity, volume/expression and other live modulators.

Initial deterministic controller state for the baseline suite:
- CC1 = 0
- CC7 = 100
- CC10 = 64
- CC11 = 127
- CC91 = 0
- CC93 = 0
- pitch wheel = center (8192)
- pitch wheel sensitivity = 2 semitones
- channel pressure = 0

Baseline real-bank matrix:
- comparable ~6 MiB FM/GM Open fixture
- comparable ~6 MiB GeneralUser GS fixture
- comparable ~6 MiB FluidR3_GM fixture
- bank 0 programs: 8, 16, 32, 50, 56, 80, 88, 96
- key 60
- velocity 96

### Conformance gates

**L0 — structural conformance (must be green before M3)**
- same playing voice count
- same actual key
- same actual velocity
- no resolver/modulator zone-count mismatch

**L1 — core effective-generator conformance (must be green before M3)**
Compare in native SoundFont units:
- vibLfoToPitch
- initialFilterFc
- initialFilterQ
- chorusEffectsSend
- reverbEffectsSend
- pan
- initialAttenuation
- coarseTune
- fineTune
- scaleTuning

Once the baseline matrix is clean, CI switches to strict-generator mode.

**L2 — dynamic controller matrix (may overlap early M3 work)**
Expand coverage to:
- velocity 32 / 96 / 127
- representative keys 48 / 60 / 72
- CC1 modulation wheel
- CC7 volume
- CC10 pan
- CC11 expression
- CC91/93 effect sends
- pitch wheel and pitch-wheel sensitivity
- channel pressure / poly pressure where the bank actually routes them

L2 must verify that event-triggered ModPlan updates match the oracle without
introducing arbitrary modulator-table scans into the audio hot loop.

## Phase C — runtime update plan

Rules:
- Key/Velocity static modulation: evaluate at NoteOn / prepared-zone activation.
- CC modulation: evaluate only when that CC changes.
- Pitch/pressure modulation: evaluate only on matching events.
- Audio render path consumes final voice parameters; it MUST NOT scan arbitrary modulator tables every sample.

Continuous DSP such as LFO, filter and modulation envelope may still run per-sample or control-rate as required; their CPU cost is measured separately from ModPlan evaluation.

## Qualification banks

- FM/GM Open SoundFont: simple baseline
- GeneralUser GS: primary real-world modulator-compliance baseline
- FluidR3_GM: broad classic-bank reference
- TimGM6mb: compact additional baseline when supplied locally/added later
- GMBK5X128: robustness only; do not derive standard semantics from it

## Exit criteria

- default + `imod` + `pmod` merge rules are unit-tested
- source curve evaluation has reference tests
- zero-amount instrument overrides behave correctly
- L0 real-bank matrix is green against the pinned effective FluidSynth oracle
- L1 core effective-generator matrix is green and enforced in CI
- deterministic ModPlans are produced for real qualification banks
- no arbitrary pmod/imod table scan exists in the per-sample audio loop
- estimated/observed ModPlan update cost is separated from continuous DSP cost
