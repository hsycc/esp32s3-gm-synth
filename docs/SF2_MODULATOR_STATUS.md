# SF2 Modulator Runtime Status

This note records the runtime-modulator coverage that is continuously exercised by the SoundFont subsets committed to this repository. It is a qualification statement for those exact checked-in assets, not a claim that arbitrary SF2 custom modulators are universally supported.

## Checked-in real-bank closure

The Phase G host gate resolves every preset in the current checked-in subsets and requires zero unsupported modulator records for each resolved program/zone.

### GeneralUser GS subset

The current subset contains 35 bank-0 presets. Its exercised modulator families are represented by the runtime engine, including:

- velocity -> initial attenuation
- velocity -> initial filter cutoff, including the SF2 2.01 cancellation form
- velocity -> modulation-envelope filter depth
- velocity -> initial filter Q
- velocity -> modulation-envelope attack
- velocity -> volume-envelope attack
- Channel Pressure -> vibrato-LFO pitch
- MIDI controller routes already materialized for channel volume/pan/expression and effects sends when present
- reverb and chorus send routes
- immutable NoteOn Key Number positive-bipolar-linear (`sourceOper 0x0203`) -> initial filter cutoff
- immutable NoteOn Velocity negative-unipolar-convex (`sourceOper 0x0902`) -> initial filter cutoff

The generic immutable NoteOn source evaluator is deliberately bounded and currently accepts NoteOn Key Number and NoteOn Velocity source identities. It does not turn arbitrary MIDI CC, amount-source, transform, or destination combinations into implicitly supported routes.

### FluidR3 GM subset

The current subset contains 23 bank-0 presets. Its effective `imod` records are the zero-amount SF2 2.01 velocity/filter cancellation pattern (`sourceOper 0x0102`, `amountSourceOper 0x0D02`, destination `initialFilterFc`). The resolver recognizes this pattern as supported semantic cancellation rather than reporting it as an unknown custom modulator.

### FM/GM Open subset

The current checked-in subset contains 67 bank-0 presets and no effective file-defined `pmod`/`imod` records. Every one of those 67 presets is required to pass strict Program Prepare with no preview-only issue and no unsupported modulator diagnostic.

This asset is **not** the final GM128 qualification bank: the checked-in subset currently has 67 melodic presets and no bank-128 drum preset. Final product qualification still requires all 128 GM melodic programs plus the standard drum path as defined in `REQUIREMENTS.md`.

## Compatibility boundary

A SoundFont can still contain source/destination/amount-source/transform identities outside the current runtime profile. Those records must remain explicit compatibility failures/preview diagnostics; they must not be silently treated as fully supported.

The compatibility contract therefore remains:

1. recognized route semantics are materialized into bounded realtime state during Program Prepare;
2. the audio path does not run a generic per-sample modulator interpreter;
3. unsupported records are counted and surfaced;
4. `ZoneModulatorResolveResult::fullySupported()` is true only when resolution succeeded and the resolved program contains zero unsupported modulator records.

This keeps the realtime path bounded while allowing compatibility support to expand route-by-route from real-bank evidence.