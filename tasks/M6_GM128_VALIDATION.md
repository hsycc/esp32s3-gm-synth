# M6 — GM128 Qualification

## Goal
Qualify the fixed GM bank and firmware as a repeatable embedded sound-module baseline.

## Automated host semantic gate

The tracked full FM/GM baseline at
`assets/sf2/full/FM_GM_SoundFont_v0_2_1_mini.sf2` is part of the consolidated
Phase G host validation. The gate exercises the real C++
`Sf2PdtaReader -> ResolvedZone -> ModulatorResolver -> PlaybackMaterializer -> ProgramPrepare`
path rather than relying only on analyzer metadata.

Required host checks:
- bank 0 programs 0–127 all StrictQualified Program Prepare successfully;
- every melodic program publishes a valid non-empty Playback snapshot;
- representative low/mid/high keys (0/60/127 at velocity 96) resolve to a playable zone;
- every program reports zero unsupported modulator records and zero Preview zones;
- bank 128 / program 0 StrictQualified Program Prepare succeeds on MIDI channel 10 (zero-based channel 9);
- standard drum notes 35–81 are all covered by prepared zones;
- each required drum note has a prepared fixed-pitch zone (`scaleTuning == 0`, no key tracking);
- full-bank changes retrigger the Phase G workflow.

Passing this host gate establishes parser/resolver/materializer/control-domain semantics for the checked-in GM bank. It does **not** by itself prove audible quality, physical realtime margin, storage timing, or guaranteed polyphony.

## Functional matrix
Test all required paths:
- melodic programs 0–127
- channel-10 drums used by the bank
- all 16 MIDI channels
- Note On / Note Off
- Program Change
- Bank Select
- pitch bend
- supported pitch-bend range RPN
- CC7 volume
- CC10 pan
- CC11 expression
- CC64 sustain
- CC91 reverb send
- CC93 chorus send
- GM reset

## Program coverage
For every program collect or automate checks for:
- representative low/mid/high notes
- low/mid/high velocities where velocity zones exist
- loop/release behavior
- obvious pitch/root-key errors
- missing/corrupt sample references

The host semantic gate covers structural Program Prepare and representative key reachability. Audio-render/reference-listening coverage and velocity/loop/release behavior remain separate qualification evidence.

## Stress suite
- dense sustained piano
- repeated same-note retrigger
- drum bursts
- pad/release-tail accumulation
- rapid Program Change
- mixed 16-channel MIDI
- worst layered/zone-heavy programs from M1
- long-running playback

## Required telemetry in reports
- active/max/admission voice count
- peak render time
- p95/p99 render time where available
- deadline misses
- underruns
- event queue drops
- invalid sample/fetch accesses
- voice steals
- internal SRAM high-water data
- PSRAM/sample-pool high-water data

## Qualification rule
A polyphony value is considered guaranteed only when it survives the relevant stress suite with zero unrecovered realtime failures and adequate measured deadline margin.

## Exit criteria
- GM128 matrix completed
- known unsupported SF2/GM behaviors documented
- stable guaranteed voice count stated from hardware evidence
- release benchmark report committed to `benchmarks/`
