# M6 — GM128 Qualification

## Goal
Qualify the fixed GM bank and firmware as a repeatable embedded sound-module baseline.

## Automated host semantic gate

The tracked full FM/GM baseline at
`assets/sf2/full/FM_GM_SoundFont_v0_2_1_mini.sf2` is validated by the dedicated
`gm128-full-bank` host workflow. The normal Phase G lane keeps smaller fixtures
as its fast regression baseline.

Required host semantic checks:
- bank 0 programs 0–127 StrictQualified successfully;
- representative low/mid/high keys and velocities resolve to playable zones;
- zero unsupported modulator records and zero Preview zones in the tracked FM bank;
- bank 128 / program 0 works on MIDI channel 10;
- GM drum notes 35–81 are covered by fixed-pitch prepared zones;
- the real resolver/materializer/Program Prepare path is used.

## Automated runtime-control gate

The full-bank runtime test exercises:

`RealtimeProgramChangeRouter -> ProgramChangeControlService -> ProgramPrepareController -> residency/publication -> paired READY snapshots -> block-boundary commit -> ACK -> RealtimeProgramEventApplier/VoicePool`.

Covered paths include full-bank melodic Program Change, channel-10 percussion,
16-channel GM Reset and post-commit NoteOn. The canonical transaction fixture
uses an oversized host arena to isolate lifetime/transaction correctness.

## Product-sized residency and capacity gates

The dedicated full-bank lane separately proves product-sized behavior:
- all bank-0 programs 0–127 switch sequentially in 4 MiB;
- all bank-128 programs 0–127 switch sequentially in 4 MiB;
- the melodic sweep exercises safe cache eviction without stale references;
- real CC0/CC32 Bank Select reaches the selected full-bank identity;
- a 128-Program-Change burst coalesces to the latest request;
- the representative 16-channel Active union is 4,310,396 aligned PCM bytes;
- worst scanned Active+READY is channel 0 / program 122 at 4,504,436 bytes;
- 4.25 MiB fails that READY admission while 4.50 MiB is the first tested arena
  that completes Prepare + READY + commit + ACK.

The firmware default stays 4 MiB until N16R8 total-PSRAM headroom with the audio
runtime is measured; the host capacity result does not by itself authorize a
product default change.

## Functional matrix

Host coverage includes:
- melodic programs 0–127
- channel-10 drums
- all 16 MIDI channels
- Note On / Note Off
- Program Change
- Bank Select
- pitch bend and pitch-bend-range RPN
- CC7 volume
- CC10 pan
- CC11 expression
- CC64 sustain
- CC91 reverb send
- CC93 chorus send
- GM reset

## Program / stress coverage

The dedicated lane covers:
- representative key/velocity reachability for every melodic program;
- missing/corrupt sample-reference rejection through StrictQualified Prepare;
- dense sustained piano;
- repeated same-note retrigger;
- drum bursts;
- rapid Program Change;
- mixed 16-channel MIDI;
- automatic discovery and runtime exercise of the largest playable NoteOn layer count;
- automatic discovery and key sweep of the largest prepared zone set;
- bounded long-release-tail accumulation and natural PublishedSample ref reclamation;
- a deterministic 1,411,200-frame / 32.0-second host soak with repeated 16-channel
  NoteOn/Off, sustain, live CC/pitch updates and drum bursts.

Host soak evidence is for semantic/lifetime/boundedness failures. It is not a
wall-clock render-performance claim.

## Required final hardware telemetry

N16R8 qualification must still record:
- active/max/admission voice count;
- peak/p95/p99 render time;
- deadline misses;
- I2S underruns;
- event queue drops;
- invalid sample/fetch accesses;
- voice steals;
- internal SRAM high-water;
- total PSRAM and sample-arena high-water;
- Program Prepare cold/warm/eviction latency;
- long-running mixed playback stability.

A polyphony value is guaranteed only when it survives the physical stress suite
with zero unrecovered realtime failures and adequate deadline margin.

## Exit criteria

- GM128 functional matrix completed;
- known unsupported SF2/GM behaviors documented;
- host 4 MiB single-channel residency/eviction gate green;
- host 16-channel Active+READY capacity bracket green;
- host mixed-16-channel / worst-program / release-tail / long-soak gates green;
- N16R8 total-memory headroom measured for the selected sample-arena size;
- stable guaranteed voice count stated from N16R8 hardware evidence;
- release benchmark report committed under `benchmarks/`.
