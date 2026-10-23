# M6 Physical Qualification Baseline — 2026-10-20

This document freezes the physical ESP32-S3 N16R8 M6 qualification evidence at commit:

`c16ff278b265018be9d2af5cc4fe3e54fe01e947`

It is the M6 closeout checkpoint and the starting performance baseline for M7 GM128 / user-SF2 release qualification.

## Target

- ESP32-S3-WROOM-1-N16R8
- 16 MiB Flash
- 8 MiB Octal PSRAM at 80 MHz
- ESP-IDF v6.1
- CPU 240 MHz
- 44.1 kHz / 16-bit audio
- 64 frames per render block
- 1451 us nominal block period
- physical qualification PCM arena: 4.50 MiB
- I2S -> ES8311 -> PA -> speaker

## Storage / residency

Internal-Flash -> PSRAM throughput remained approximately 5.8–6.3 MiB/s across the 4/8/16/32/64 KiB sweep. The 32 KiB source chunk remains the practical baseline.

Program Prepare on the tracked physical test bank measured:

- cold initial Prepare: `102007 us`;
- warm same-program Prepare: `107 us`;
- prime-cache high-water: `4633820` bytes;
- eviction Program Change: `144955 us`;
- safe evictions: `9`;
- evicted bytes: `847604`;
- capacity retries: `9`;
- final arena used: `4602060` bytes;
- stale residency references: `0`.

The physical run therefore demonstrates that the 4.50 MiB qualification arena coexists with the complete qualification runtime on N16R8. This does not by itself change the normal firmware default arena policy.

## Clean real-SF2 path

The real-SF2 clean path passed the automated runtime and I2S gates:

- events applied: `16`;
- voices started: `8`;
- missing programs: `0`;
- unmatched zones: `0`;
- resolve overflows: `0`;
- voice-start failures: `0`;
- clipped samples: `0`;
- queue remaining: `0`;
- I2S hard-gate failures: `0`.

Clean-path render timing was `115 / 164 / 926 us` min/avg/max with zero render soft overruns and zero accumulated realtime backlog.

This is automated functional/audio-path evidence. Subjective listening remains separate release-quality evidence and is not inferred from timing logs.

## Normal / full-FX physical polyphony

The sustained normal/full-FX sweep remains a strict realtime-boundary measurement:

| Voices | Avg render | Max render | Deadline misses | Result |
| ---: | ---: | ---: | ---: | --- |
| 16 | 896 us | 904 us | 0 | stable |
| 20 | 1081 us | 1091 us | 0 | stable |
| 24 | 1268 us | 1278 us | 0 | stable |
| 28 | 1455 us | 1465 us | 121/128 | unstable |
| 32 | 1641 us | 1650 us | 128/128 | unstable |

M6 normal/full-FX physical maximum: **24 Voices**.

The first tested unstable step is **28 Voices**. The software hard cap remains 32 Voices; the hard cap is not a guaranteed realtime polyphony claim.

## Pathological heavy-DSP class

The all-features-on heavy-DSP characterization measured:

| Voices | Avg render | Max render | Result |
| ---: | ---: | ---: | --- |
| 1 | 295 us | 303 us | stable |
| 2 | 439 us | 446 us | stable |
| 4 | 727 us | 736 us | stable |
| 8 | 1301 us | 1312 us | stable |
| 12 | 1878 us | 1889 us | first unstable |

Pathological heavy-DSP stable maximum: **8 Voices**.

This is a deliberately hostile complexity class combining resonant/dynamic filtering, vibrato LFO, modulation LFO, modulation envelope and full FX. It is a characterization result, not the normal advertised polyphony.

## Production-shaped music stress

The final deterministic music timeline covered `118` events over `16384` measured blocks and exercised sustain, release, percussion, pitch, CC, Program Change handoff and GM Reset.

Semantic result:

- queued / attempted / applied events: `118 / 118 / 118`;
- rejected events: `0`;
- voices resolved / started: `40 / 40`;
- voice-start failures: `0`;
- peak active Voices: `24`;
- Program Change latest-program state: valid;
- queue remaining: `0`.

Realtime timing:

- render min/avg/max: `130 / 754 / 1896 us`;
- producer-work min/avg/max: `133 / 757 / 1899 us`;
- render soft overruns: `5`;
- producer-work soft overruns: `6`;
- maximum consecutive producer-work overruns: `2`;
- maximum accumulated backlog: `448 us`;
- maximum backlog blocks: `1`;
- maximum recovery horizon: `19` blocks;
- final backlog: `0 us`;
- DMA window: `11610 us`;
- minimum remaining DMA margin: `11162 us`;
- backlog exceeded DMA window: `0`;
- active synth send-queue overflows: `0`;
- producer stalls over DMA window: `0`;
- write timeouts: `0`;
- partial writes: `0`;
- clipped samples: `0`;
- hard-gate failure flags: `0x00000000`.

### Soft deadline versus hard realtime health

At 44.1 kHz / 64 frames, 1451 us remains the useful per-block soft timing target. It is not a hard failure by itself for isolated event bursts when buffered audio has adequate reserve and the producer recovers.

M6 hard realtime qualification is DMA/backlog-aware. A production-shaped timeline fails for unrecovered backlog, DMA-window exhaustion, active playback queue overflow, write failure, clipping or insufficient stack/runtime safety, rather than merely because one transient block exceeded 1451 us.

The sustained polyphony sweeps remain strict boundary tests because a workload that continuously consumes at or above the block period cannot recover indefinitely.

## Worst transient blocks

The aligned pure-render profiler found exactly the same five render soft overruns reported by the I2S qualification:

1. 24 simultaneous NoteOn events: `1896 us`;
2. 12 NoteOff + 1 CC with 24 active / 8 releasing: `1671 us`;
3. 16 NoteOn + 2 CC at timeline block 0: `1595 us`;
4. 9 CC events: `1509 us`;
5. event-free 24 active / 8 releasing block: `1452 us`.

The first four are control/event bursts. They remain useful M7 regression metrics, but they are not M6 blockers because the measured backlog remains bounded and fully recovers.

## M6 decision

M6 physical product qualification is **COMPLETE** at `c16ff278` with the following baseline:

- normal/full-FX nominal physical polyphony: **24 Voices**;
- first normal sustained unstable step: **28 Voices**;
- pathological heavy-DSP stable maximum: **8 Voices**;
- first pathological unstable step: **12 Voices**;
- 32-Voice software hard cap retained;
- production-shaped 24-Voice mixed music stress: **PASS** with hard-gate failure flags `0` and final backlog `0`.

Further optimization of the ~1.9 ms same-block NoteOn burst is an M7 performance/headroom opportunity, not an M6 exit requirement.
