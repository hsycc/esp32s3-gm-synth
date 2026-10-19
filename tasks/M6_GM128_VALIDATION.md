# M6 — GM128 Qualification

Status: **COMPLETE**

M6 physical qualification is closed at:

`c16ff278b265018be9d2af5cc4fe3e54fe01e947`

The authoritative physical closeout is committed as:

`benchmarks/M6_PHYSICAL_QUALIFICATION_BASELINE_2026-10-20.md`

## Host / QEMU qualification

The dedicated `gm128-full-bank` lane establishes the tracked full-bank semantic, residency and lifetime baseline:

- [x] Bank 0 programs 0–127 StrictQualified;
- [x] Bank 128 percussion qualification and GM drum-key coverage;
- [x] all tracked full-bank presets StrictQualified;
- [x] Program Change Prepare / READY / commit / ACK;
- [x] channel-10 percussion runtime path;
- [x] 16-channel GM Reset transaction;
- [x] Note On / Note Off;
- [x] Bank Select;
- [x] pitch bend and supported bend-range RPN;
- [x] CC7 / CC10 / CC11 / CC64 / CC91 / CC93;
- [x] 4 MiB melodic 0–127 Program Change sweep with safe-cache eviction;
- [x] 4 MiB Bank-128 0–127 Program Change sweep;
- [x] rapid latest-wins Program Change coalescing;
- [x] dense/retrigger/drum functional stress;
- [x] representative mixed 16-channel runtime behavior;
- [x] 16-channel Active+READY capacity bracket;
- [x] automatic largest-zone / maximum-layer discovery;
- [x] natural release-tail lifetime/reclamation;
- [x] deterministic 32-second mixed host soak.

The representative 16-channel Active set requires `4,310,396` aligned PCM bytes. The worst scanned Active+READY union requires `4,504,436` bytes; 4.50 MiB is the first tested host arena that completes Prepare + READY + commit + ACK.

## Physical N16R8 evidence

The completed physical qualification demonstrated:

- [x] 4.50 MiB PCM arena on N16R8 with complete runtime headroom;
- [x] internal-Flash -> PSRAM storage characterization;
- [x] cold/warm/eviction Program Prepare telemetry;
- [x] clean real-SF2 runtime over physical I2S / ES8311 / PA path;
- [x] normal/full-FX sustained polyphony boundary;
- [x] pathological heavy-DSP boundary;
- [x] deterministic sustain/release/percussion/CC/Program Change/GM Reset music stress;
- [x] DMA/backlog-aware realtime hard gate;
- [x] zero active playback queue overflow, write timeout, partial write and clipping in the final stress run;
- [x] zero stale residency reference faults.

### Physical product envelope

Normal/full-FX sustained workload:

- 16 Voices: stable;
- 20 Voices: stable;
- **24 Voices: stable** (`1268 us` avg, `1278 us` max);
- 28 Voices: first unstable tested step;
- 32 Voices: unstable;
- normal/full-FX nominal physical result: **24 Voices**.

Pathological all-features-on workload:

- 1 / 2 / 4 / **8 Voices**: stable;
- 12 Voices: first unstable tested step;
- pathological heavy-DSP result: **8 Voices stable, 12 first unstable**.

The software hard cap remains 32 Voices and is not a guaranteed realtime claim.

## Realtime qualification policy

At 44.1 kHz / 64 frames, approximately `1451 us` remains a useful **soft** per-block timing target for the production-shaped continuous I2S stress.

A transient event block above 1451 us is not by itself a hard failure if buffered audio remains safe and the producer recovers. The hard physical gate now evaluates:

- accumulated backlog versus the DMA window;
- final unrecovered backlog;
- producer stalls over the DMA window;
- active synth/tone queue overflow;
- write timeout / partial write;
- clipping;
- stack/runtime safety.

The final 16384-block music stress measured:

- render soft overruns: `5`;
- max render: `1896 us`;
- max backlog: `448 us`;
- DMA window: `11610 us`;
- minimum DMA margin: `11162 us`;
- final backlog: `0 us`;
- hard-gate flags: `0x00000000`;
- result: **PASS**.

Sustained polyphony sweeps remain strict boundary tests because continuously consuming the entire block period cannot be hidden indefinitely by buffering.

## Exit criteria

- [x] GM128 host functional matrix complete;
- [x] host 4 MiB single-channel residency/eviction gate green;
- [x] host 16-channel Active+READY capacity bracket green;
- [x] host worst-program/release-tail/long-soak gates green;
- [x] N16R8 total-memory headroom measured with the 4.50 MiB qualification arena;
- [x] cold/warm/eviction Program Prepare measured physically;
- [x] stable normal/full-FX physical Voice count established at 24;
- [x] heavy-DSP physical complexity boundary established at 8 stable / 12 first unstable;
- [x] final production-shaped I2S music stress passes the DMA/backlog-aware hard gate;
- [x] final physical release benchmark committed under `benchmarks/`.

Known unsupported / cross-bank SF2 compatibility behavior is now an **M7 release-qualification** concern rather than an M6 blocker. Continue in `tasks/M7_GM128_USER_SF2_RELEASE.md`.
