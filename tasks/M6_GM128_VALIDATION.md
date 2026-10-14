# M6 — GM128 Qualification

## Goal
Qualify the fixed GM bank and firmware as a repeatable embedded sound-module baseline.

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
