# M2.5 — SF2 Modulator Resolver（调制器解析器）

## Status

**L0 + L1 complete and enforced in CI.**

Pinned baseline result against FluidSynth 2.6.1 commit
`71e85b2ca6bf48641ba7e2261d8f2640ae473773`:

- 3 comparable ~6 MiB public SF2 fixtures
- 8 bank-0 programs per fixture
- 24 real-bank cases total
- key 60 / velocity 96
- structural failures: **0**
- generator failures: **0**
- maximum observed generator delta: **5.34015444486613e-06 cb** on `initialAttenuation`
- CI acceptance tolerance: `1e-3`

The `sf2-conformance` workflow now runs with `--strict-generators`; future
changes that regress these effective generator values fail CI.

## Goal

Support mainstream SoundFont dynamic-control semantics without interpreting arbitrary `pmod` / `imod` tables inside the per-sample audio hot loop.

## Terminology

- Default Modulator（默认调制器）: SoundFont-defined controller routes that exist even if the file contains no custom modulator records.
- `imod` / Instrument Modulator（乐器级调制器）: instrument-zone rules which may overwrite identical defaults.
- `pmod` / Preset Modulator（预置级调制器）: preset-zone rules which add to instrument/default modulation.
- ModPlan（调制计划）: pre-resolved event/control-rate representation consumed by runtime voices.

## Phase A — identity and merge semantics

Implemented:
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

## Phase B — source mapping/evaluation

Implemented and reference-tested:
- Linear（线性）
- Concave（凹曲线）
- Convex（凸曲线）
- Switch（开关）
- Positive/Negative（正向/反向）
- Unipolar/Bipolar（单极/双极）
- primary source × amount source
- standard transform operator (`transform=0`)

Non-zero transforms are rejected explicitly rather than interpreted silently.

## Runtime compatibility policy

Native analysis and playback compatibility are intentionally separate:

- `ResolvedZone.generators_native` keeps the literal SF2 hierarchy result.
- `strict_sf2` evaluates the literal/default SF2 modulation model.
- `realworld_sf2` is the default runtime profile and mirrors the two legacy
  compatibility behaviors required by the qualification banks:
  - SoundFont `initialAttenuation` generator base uses the EMU/FluidSynth
    `0.4` compatibility factor.
  - the exact standard default Velocity → FilterFc modulator identity is
    suppressed, matching current FluidSynth compatibility behavior.

This split prevents real-world playback quirks from corrupting SF2 analysis.

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

**L0 — structural conformance: GREEN**
- same playing voice count
- same actual key
- same actual velocity
- no resolver/modulator zone-count mismatch

**L1 — core effective-generator conformance: GREEN + STRICT CI**
Compared in native SoundFont units:
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

**L2 — dynamic controller matrix (continues in parallel with early M3)**
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

Completed for L0/L1:
- default + `imod` + `pmod` merge rules are unit-tested
- source curve evaluation has reference tests
- zero-amount instrument overrides behave correctly
- L0 real-bank matrix is green against the pinned effective FluidSynth oracle
- L1 core effective-generator matrix is green and enforced in CI
- deterministic ModPlans are produced for real qualification banks
- no arbitrary pmod/imod table scan exists in the per-sample audio loop

Remaining M2.5 follow-up:
- L2 dynamic event matrix
- measure ModPlan event-update cost separately from continuous DSP cost
