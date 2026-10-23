# M7 Physical Long-Run Qualification — 2026-10-20

Release-image checkpoint: `22b48b2f8ef58d01c3c0c39ddea47c7d40f330a9`

Hardware/profile:

- ESP32-S3-WROOM-1-N16R8, 240 MHz;
- 16 MiB flash, DIO 80 MHz;
- 8 MiB octal PSRAM, 80 MHz;
- ESP-IDF v6.1;
- ES8311 physical mono I2S path;
- 44.1 kHz, 64 frames/block (`1451 us` nominal block period);
- 8 x 64-frame DMA descriptors (`11610 us` configured DMA window);
- 4.50 MiB PCM residency arena;
- `CONFIG_GM_SYNTH_I2S_QUAL_MEASURE_BLOCKS=131072`.

The physical run was built with `scripts/build_m7_release_idf.sh`, flashed from `build-m7-release`, and reported application version `22b48b2`.

## Clean real-SF2 long run

The first 131072-block qualification ran the real-SF2 dry cue through the production prepare/residency/realtime/I2S path.

| Metric | Result |
|---|---:|
| measured blocks | 131072 |
| render min / avg / max | 115 / 122 / 935 us |
| producer min / avg / max | 118 / 126 / 939 us |
| render soft overruns | 0 |
| producer soft overruns | 0 |
| max backlog | 0 us |
| final backlog | 0 us |
| min DMA margin | 11610 us |
| active synth queue overflows | 0 |
| producer stalls | 0 |
| write timeouts | 0 |
| partial writes | 0 |
| clipped samples | 0 |
| hard-gate flags | `0x00000000` |
| stack headroom gate | PASS |
| result | PASS |

The clean-SF2 result applied all 16 scheduled events, started 8 voices, and reported zero missing-program, no-match, resolve-overflow, voice-start-failure, clipping, or residual-queue faults.

## Product envelope re-check

The same run re-characterized the established M6 product boundaries before the production-shaped soak:

| Workload | Stable maximum | First tested unstable step |
|---|---:|---:|
| normal / full-FX | 24 voices | 28 voices |
| pathological heavy-DSP | 8 voices | 12 voices |

Representative physical timing:

- normal/full-FX 24 voices: avg `1268 us`, max `1278 us`, zero misses;
- normal/full-FX 28 voices: avg `1455 us`, max `1465 us`, 121/128 misses;
- heavy-DSP 8 voices: avg `1301 us`, max `1318 us`, zero misses;
- heavy-DSP 12 voices: avg `1878 us`, max `1890 us`, 128/128 misses.

These results preserve the established product envelope: nominal 24-voice normal/full-FX operation and 8-voice pathological-heavy operation.

## Production-shaped 131072-block long run

The second long run armed 118 events across sustain/release, percussion, pitch, CC, Program Change handoff, and GM Reset. The measured stream was 8388608 frames (~190.22 s).

### I2S / realtime health

| Metric | Result |
|---|---:|
| measured blocks | 131072 |
| render min / avg / max | 130 / 737 / 1870 us |
| producer min / avg / max | 133 / 740 / 1874 us |
| cycle min / avg / max | 911 / 1440 / 2011 us |
| render soft overruns | 4 |
| producer soft overruns | 4 |
| max consecutive work overruns | 1 |
| max backlog | 423 us / 1 block |
| max backlog recovery horizon | 19 blocks |
| final backlog | 0 us |
| minimum DMA margin | 11187 us |
| backlog exceeded DMA window | 0 |
| producer stalls | 0 |
| write timeouts | 0 |
| partial writes | 0 |
| active synth queue overflows | 0 |
| clipped samples | 0 |
| stack headroom gate | PASS |
| hard-gate flags | `0x00000000` |
| I2S qualification | PASS |

The four >1451-us renderer blocks were transient soft diagnostics, not hard realtime failures. The worst accumulated backlog was only 423 us, recovered completely, never approached the 11610-us DMA window, and ended at zero.

The retained worst renderer block was `1870 us` at stream block 57344, where 24 NoteOn events arrived in one block and 24 voices became active with FX sends and realtime controls. This is the expected burst case that motivates the DMA/backlog-aware hard gate rather than treating every isolated 1451-us miss as an underrun.

### Music-stress semantics

- queued / attempted / applied events: `118 / 118 / 118`;
- rejected events: `0`;
- NoteOn / NoteOff / CC: `40 / 28 / 23`;
- pitch bend / pressure: `1 / 1`;
- All Notes Off / All Sound Off: `6 / 2`;
- peak active voices: `24`;
- sustain deferred / released: `16 / 16`;
- resolved / started voices: `40 / 40`;
- voice start failures: `0`;
- percussion unavailable: `0`;
- no matching zones: `0`;
- resolve overflows: `0`;
- Program Change events: `24`;
- GM Reset events: `1`;
- published requests: `40`, rejected requests: `0`;
- handoffs taken: `16`;
- latest program state: correct;
- queue remaining / full rejects / order rejects: `0 / 0 / 0`;
- `music_stress: PASS`;
- `effects_bench: PASS final_refs=0`.

Final residency state remained coherent:

- arena capacity `4718592` bytes;
- used `4602060` bytes;
- high-water `4633820` bytes;
- resident samples `54`;
- expected allocation retries/failures `13` under cache-pressure qualification;
- stale-reference faults `0`.

## Qualification decision

**M7 automated physical long-run: PASS.**

This closes the automated physical DMA/backlog/write/queue/clipping/stability criterion for checkpoint `22b48b2f` on the N16R8 reference hardware. It does not replace subjective audible evidence. The current ES8311 qualification path is mono, so spatial stereo/pan correctness remains a host/stereo-PCM evidence item rather than a single-speaker listening claim.
