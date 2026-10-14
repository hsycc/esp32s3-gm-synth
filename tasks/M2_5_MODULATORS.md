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
- TimGM6mb: compact modulator-heavy baseline
- FluidR3_GM: broad classic-bank reference
- GMBK5X128: robustness only; do not derive standard semantics from it

## Exit criteria

- default + `imod` + `pmod` merge rules are unit-tested
- source curve evaluation has reference tests
- zero-amount instrument overrides behave correctly
- a real GeneralUser GS/TimGM note can produce a deterministic ModPlan
- no arbitrary pmod/imod table scan exists in the per-sample audio loop
- estimated/observed ModPlan update cost is separated from continuous DSP cost
