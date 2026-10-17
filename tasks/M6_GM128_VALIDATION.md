# M6 — GM128 Qualification

## Goal

Qualify the fixed GM bank and firmware as a repeatable embedded sound-module baseline.

## Host / QEMU status

The host-side GM128 semantic, residency and lifetime qualification is now substantially complete and automated in the dedicated `gm128-full-bank` lane.

Completed gates:

- [x] bank 0 programs 0–127 StrictQualified;
- [x] bank 128 percussion qualification and GM drum-key coverage;
- [x] all 5,760 actual presets in the tracked full FM bank StrictQualified;
- [x] representative key/velocity coverage;
- [x] Program Change Prepare/READY/commit/ACK transaction;
- [x] channel-10 percussion runtime path;
- [x] 16-channel GM Reset transaction;
- [x] Note On / Note Off;
- [x] Bank Select;
- [x] pitch bend and supported pitch-bend-range RPN;
- [x] CC7 / CC10 / CC11 / CC64 / CC91 / CC93;
- [x] 4 MiB melodic 0–127 Program Change sweep with real safe-cache eviction;
- [x] 4 MiB bank-128 0–127 Program Change sweep;
- [x] rapid Program Change latest-wins coalescing;
- [x] dense piano, same-note retrigger and drum bursts;
- [x] representative mixed 16-channel runtime behavior;
- [x] 16-channel Active+READY capacity bracket;
- [x] automatic largest-zone / maximum-NoteOn-layer discovery;
- [x] natural release-tail accumulation and published-sample ref reclamation;
- [x] deterministic 32-second / 1,411,200-frame mixed host soak.

Committed evidence:

- `benchmarks/GM128_HOST_QUALIFICATION_BASELINE_2026-10-18.md`
- `docs/GM128_FULL_BANK_QUALIFICATION.md`

Key host capacity result:

- representative 16-channel Active: `4,310,396` aligned PCM bytes;
- worst scanned Active+READY: `4,504,436` bytes at channel 0 / program 122;
- 4.25 MiB: Active fits but worst READY fails;
- 4.50 MiB: first tested arena that completes Prepare + READY + commit + ACK.

The firmware default remains 4 MiB until physical N16R8 total-memory measurements prove a larger sample arena leaves adequate reserve for the complete audio runtime.

## Remaining N16R8 release qualification

The remaining M6 work is primarily physical hardware qualification.

### Memory

- [ ] boot and allocate candidate 4.5 MiB PCM arena on the target N16R8 board;
- [ ] record total/free/minimum/largest-free PSRAM before and after audio-runtime bring-up;
- [ ] record internal SRAM free/minimum/largest block;
- [ ] load the representative 16-channel Active set and worst READY transition;
- [ ] confirm commit/retire returns memory to the expected steady state;
- [ ] verify Voice/DSP/FX/audio buffers, task stacks and diagnostics still retain safe headroom.

### Program Prepare / storage

- [ ] cold Program Prepare latency and loaded bytes;
- [ ] warm/reused Program Prepare latency;
- [ ] safe-cache eviction latency and bytes;
- [ ] rapid Program Change Prepare/commit/retire timing;
- [ ] confirm all asset I/O remains outside the audio task.

### Realtime audio matrix

At 44.1 kHz / 64 frames measure at minimum:

- [ ] 1 / 4 / 8 / 12 / 16 Voices;
- [ ] 18 Voices if reachable;
- [ ] 20 Voices if reachable;
- [ ] dry path;
- [ ] reverb enabled;
- [ ] chorus enabled;
- [ ] filter/modulation paths enabled as applicable;
- [ ] worst known program / stress pattern.

Record:

- [ ] render min / avg / p95 / p99 / max;
- [ ] active/max/admission Voice count;
- [ ] deadline misses;
- [ ] I2S underruns;
- [ ] event queue depth/high-water and drops;
- [ ] invalid sample/fetch accesses;
- [ ] Voice steals / steal-tail activity;
- [ ] internal SRAM high-water;
- [ ] PSRAM/sample-arena high-water.

### Physical stress

- [ ] dense sustained piano with pedal/retrigger;
- [ ] repeated same-note retrigger;
- [ ] drum bursts;
- [ ] release-tail/pad accumulation;
- [ ] mixed 16-channel MIDI;
- [ ] repeated Program Change between materially different working sets;
- [ ] long-running mixed playback.

## Qualification rule

A polyphony value is guaranteed only when the physical target survives the relevant stress suite with zero unrecovered realtime failures and adequate measured deadline margin. Host/QEMU timing is regression evidence only and cannot establish guaranteed polyphony.

## Exit criteria

- [x] GM128 host functional matrix completed;
- [x] host 4 MiB single-channel residency/eviction gate green;
- [x] host 16-channel Active+READY capacity bracket green;
- [x] host worst-program/release-tail/long-soak gates green;
- [ ] known unsupported SF2/GM behaviors consolidated for release documentation;
- [ ] N16R8 total-memory headroom measured for the selected production arena;
- [ ] stable guaranteed Voice count stated from N16R8 hardware evidence;
- [ ] final physical release benchmark report committed under `benchmarks/`.
