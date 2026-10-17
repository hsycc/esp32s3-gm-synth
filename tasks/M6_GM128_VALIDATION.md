# M6 — GM128 Qualification

## Goal
Qualify the fixed GM bank and firmware as a repeatable embedded sound-module baseline.

## Automated host semantic gate

The tracked full FM/GM baseline at
`assets/sf2/full/FM_GM_SoundFont_v0_2_1_mini.sf2` is validated by the dedicated
`gm128-full-bank` host workflow. The normal Phase G lane keeps the 6 MiB fixtures
as its fast regression baseline so ordinary commits do not repeatedly pay for a
14+ MB full-bank scan.

The full-bank gate exercises the real C++
`Sf2PdtaReader -> ResolvedZone -> ModulatorResolver -> PlaybackMaterializer -> ProgramPrepare`
path rather than relying only on analyzer metadata.

Required host checks:
- bank 0 programs 0–127 all StrictQualified Program Prepare successfully;
- every melodic program publishes a valid non-empty Playback snapshot;
- representative low/mid/high keys and low/mid/high velocities resolve to playable zones;
- every program reports zero unsupported modulator records and zero Preview zones;
- bank 128 / program 0 StrictQualified Program Prepare succeeds on MIDI channel 10 (zero-based channel 9);
- standard drum notes 35–81 are all covered by prepared zones;
- each required drum note has a prepared fixed-pitch zone (`scaleTuning == 0`, no key tracking);
- full-bank changes retrigger the dedicated GM128 workflow.

Passing this host gate establishes parser/resolver/materializer/control-domain semantics for the checked-in GM bank. It does **not** by itself prove audible quality, physical realtime margin, storage timing, or guaranteed polyphony.

## Automated host runtime-control gate

The full-bank integration test also exercises the production-shaped transaction across
`RealtimeProgramChangeRouter -> ProgramChangeControlService -> ProgramPrepareController -> sample residency/publication -> paired READY snapshots -> block-boundary commit -> ACK -> RealtimeProgramEventApplier/VoicePool`.

Required runtime-control checks:
- an initially activated full-bank melodic snapshot can immediately resolve NoteOn and start a Voice from the published resident sample;
- channel 1 can change from program 0 to program 127 through the complete realtime/control/commit transaction and then start a Voice from program 127;
- MIDI Channel 10 remains mapped to SF2 bank 128 through the same transaction and can start the standard kick-drum note;
- GM Reset emits 16 latest-wins Program Change requests, all 16 channels prepare compatible residency/playback READY generations, all 16 commit at one audio-block boundary, and all 16 ACK/retire their old generations;
- after the GM Reset commit, every channel can independently start a NoteOn using its active snapshot, with Channel 10 using the bank-128 drum snapshot;
- no missing-active-program, percussion-fallback, residency-prepare, playback-prepare, or paired-publication invariant failure is accepted.

The canonical runtime-control test keeps an intentionally oversized 16 MiB host
arena so it isolates transaction/lifetime correctness from product-capacity
admission.

## Product-sized residency and stress gate

The dedicated full-bank lane separately exercises a **4 MiB sample arena** so
capacity/eviction behavior is not hidden by the oversized transaction fixture.

Required checks:
- bank 0 programs 0–127 complete sequential real Program Change transactions in 4 MiB;
- bank 128 programs 0–127 complete sequential real Program Change transactions in 4 MiB;
- Active + READY pinning never evicts protected samples;
- cache pressure uses only reclaimable samples and produces no stale references or publication conflicts;
- real CC0/CC32 Bank Select feeds a full-bank Program Change request;
- a burst of 128 Program Changes is coalesced to the latest request before control-domain Prepare;
- a representative 15-melodic-plus-drum workload can be prepared simultaneously in the oversized host functional arena and start all 16 MIDI channels without sample-acquire faults;
- dense piano, sustain/release, repeated same-note retrigger and GM drum bursts remain bounded by the VoicePool and do not create invalid sample accesses.

This gate reports sample-arena high-water, cache eviction/retry metrics, peak
voices, start failures and publication acquire misses. It is a host semantic and
capacity baseline, not a claim about ESP32-S3 render timing.

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

The host semantic gate covers structural Program Prepare and representative key/velocity reachability. Audio-render/reference-listening coverage and exhaustive loop/release behavior remain separate qualification evidence.

## Stress suite
Automate or measure:
- dense sustained piano
- repeated same-note retrigger
- drum bursts
- pad/release-tail accumulation
- rapid Program Change
- mixed 16-channel MIDI
- worst layered/zone-heavy programs from full-bank qualification
- long-running playback

Host coverage is appropriate for semantic/lifetime/boundedness failures. Physical
render-margin claims remain N16R8-only.

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
- GM128 functional matrix completed
- known unsupported SF2/GM behaviors documented
- host 4 MiB residency/eviction stress gate green
- host mixed-16-channel semantic stress gate green
- stable guaranteed voice count stated from N16R8 hardware evidence
- release benchmark report committed to `benchmarks/`
