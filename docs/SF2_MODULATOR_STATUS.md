# SF2 Modulator Runtime Status

This note records the runtime-modulator coverage that is continuously exercised by the SoundFont assets committed to this repository. It is a qualification statement for those exact checked-in assets, not a claim that arbitrary SF2 custom modulators are universally supported.

## Checked-in real-bank closure

The Phase G host gate resolves every preset in the compact compatibility subsets and requires zero unsupported modulator records for each resolved program/zone. The tracked full FM/GM source is additionally used as the GM128 + standard-drum Program Prepare baseline.

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

### FM/GM Open compact subset

The 6 MiB checked-in subset contains 67 bank-0 presets and no effective file-defined `pmod`/`imod` records. Every one of those 67 presets is required to pass strict Program Prepare with no preview-only issue and no unsupported modulator diagnostic. It remains the fast parser/resolver fixture used by storage/QEMU tests.

### FM/GM Open full GM baseline

The repository also tracks `assets/sf2/full/FM_GM_SoundFont_v0_2_1_mini.sf2` as an ordinary Git file. Phase G uses that full source for the GM semantic gate rather than treating the 67-preset subset as final GM coverage.

The full-bank gate requires:
- bank 0 programs 0–127 all StrictQualified Program Prepare successfully;
- representative low/mid/high notes resolve to prepared zones for every melodic program;
- bank 128 / program 0 prepares successfully as the standard drum path;
- required GM drum notes 35–81 are covered by fixed-pitch prepared zones;
- every prepared program reports zero unsupported modulator records and zero Preview zones.

This is a host semantic qualification of the checked-in bank. It does not replace physical N16R8 audio/deadline/polyphony qualification or listening/reference-render checks.

## Compatibility boundary

A SoundFont can still contain source/destination/amount-source/transform identities outside the current runtime profile. Those records must remain explicit compatibility failures/preview diagnostics; they must not be silently treated as fully supported.

The compatibility contract therefore remains:

1. recognized route semantics are materialized into bounded realtime state during Program Prepare;
2. the audio path does not run a generic per-sample modulator interpreter;
3. unsupported records are counted and surfaced;
4. `ZoneModulatorResolveResult::fullySupported()` is true only when resolution succeeded and the resolved program contains zero unsupported modulator records.

This keeps the realtime path bounded while allowing compatibility support to expand route-by-route from real-bank evidence.