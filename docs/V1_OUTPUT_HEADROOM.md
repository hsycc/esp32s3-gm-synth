# Optional output peak protection and two-channel replay

## Scope and defaults

PR #26 merged as `4b097e8bd65e86893e343943342b51e42377c03d`. Its final
reviewed head `5d02b4b` passed 12 workflows / 24 checks and all five downloaded
artifacts were verified. The merge triggered two main workflows, both successful:
`host-tests` / `40641662730` and `esp-idf-build` / `40641662879`.
That batch supplied the concurrent actors and I2S path. It also exposed a real
digital clipping failure in the unchanged FM Program 16 / 24-Voice workload.

This batch adds an explicit output candidate and a reproducible two-channel
musical phrase. The normal firmware entry and all optional feature defaults
remain **OFF**. `LoopRealtimeConfig::output_mode` defaults to `Bypass`;
`Peak64` requires a separate explicit selection. Automatic loop analysis and
output protection are independent switches. Neither mode changes the frozen
`prepared-loop-review-v3-source-matched-peak` policy, Voice admission, velocity,
Master 32767, CC91=40/CC93=20 baseline, DAC -12 dB, DMA 8x64 or watchdogs.

The original `bb31908b` hardware baseline, actual `bb31908-dirty` banner and
accepted dirty decision remain intact. Their 24 normal / 8 heavy performance
evidence does not qualify this new enabled path.

## Where the candidate runs

The scheduled renderer still renders every event at its original frame,
accumulates Voices and FX in 32-bit buffers, and applies the existing master.
Only an explicitly installed experimental output callback replaces the final
integer PCM conversion. With no callback, the original renderer body remains
exact. The compile-time-disabled renderer object is byte-compared against
pre-change main, and complete PCM/counter traces are compared for the old,
current default and current optional-ABI-without-hook builds.

`OutputPeakProtector` has one fixed 128-element int32 pending buffer (512 bytes),
fixed scalar state and no allocation, locks, I/O, logging or clock calls. The
mode is immutable for a stream. The callback can only be configured before
the first rendered block. The public standalone processor rejects invalid
block sizes, pointers, gains and calls after terminal flush without changing
state or output. Input/output buffers must not alias.

### Digital bound and gain behavior

Each block's maximum absolute post-master sample is measured across **both**
channels. Absolute value uses a wider integer so `INT32_MIN` is defined.
The linked Q30 gain cap is the smaller of unity and
`floor(32767 * 2^30 / block_peak)`. Unity avoids the variable division entirely.

The processor outputs the preceding block. Its gain starts at the previous
block's ending value and ramps to the minimum of the pending block's cap, the
incoming block's cap, and the permitted release value. The previous iteration
already bounded the start by the pending cap. Both ramp endpoints therefore
bound that entire pending block, and the same gain is used for L/R at every
frame. A quotient/remainder ramp reaches its exact endpoint without per-frame
variable division. Multiplication and rounding use defined signed 64-bit
arithmetic; there is no reliance on signed right-shift behavior.

Attack can decrease over the 64-frame block preceding a peak. Release increases
by at most `2^30/64` per block: 4,096 frames, approximately 92.88 ms at 44.1 kHz,
for a full zero-to-unity rise. Smaller reductions recover proportionally sooner.
The ceiling is the symmetric digital sample magnitude 32,767. This is **not**
an oversampled true-peak limiter or a guarantee against codec, PA, speaker or
inter-sample overload. Its sound, transient response and pumping remain subject
to listening; numerical protection is not audible approval.

### Delay and end of stream

Peak mode outputs one initial silent block. It adds exactly **64 frames** of
output latency, about 1.45 ms at 44.1 kHz, to the rest of the system's latency.
Before requesting stop, the audio owner calls `audioDrainOutput` and writes
the single pending output block to the same sink. This terminal drain is also
included in the output checksum. It does not create a new synthesized block
or advance MIDI/input time. Further input rendering is forbidden once drained.

For a normally completed stream:

| Field | Bypass | Peak64 |
|---|---:|---:|
| Input frames / rendered blocks | Original count | Original count |
| Extra latency | 0 | 64 frames |
| Terminal drain | 0 | 64 frames |
| Output frames, including initial silence | Input frames | Input frames + 64 |

A hard-aborted stream can lack its terminal drain; it must not pass the normal
completion gate. Stop still requires both owners' acknowledgments and their
final resource-access joins before graph/storage/PCM destruction.

## Three independent observations

1. `raw_peak_abs` and `raw_overload_samples` describe post-master input before
   protection, including the original negative workload. In bypass, these
   reuse the historical renderer counters (its peak field saturates at
   `INT32_MAX` for extreme values); legal tested Voice mixes are well below that.
2. `gain_min_q30`, `limited_frames` and `latency_frames` show how much processing
   actually happened. Priming silence is not counted as limited audio.
3. `post_peak_abs` is the magnitude **before final hard conversion**, and
   `post_hard_clipped` counts samples outside int16 range at that conversion.
   Bypass can consequently report a post peak above 32,767. Peak mode's bound
   is tested independently, including full-range int32 inputs.

`raw_headroom=FAIL` remains visible even if `digital_headroom=PASS`. Integration,
strict sustained-cohort timing, musical replay and digital output have separate
gates. A deliberately collected bypass failure is evidence of a failed audio
configuration, not a qualification pass. The collector cannot use that
exception to admit a runtime, memory, queue, DMA, timing or ownership failure.

## Unchanged real-bank regression

The pinned native SF2 remains 5,898,214 bytes, SHA-256
`9650eb1c2202e2d92aa3be155e4e895cf6531a241c8ea413fc09e7f9159d4a62`.
Program 16, keys 48–71, velocity 64, CC91=40, CC93=20, Master 32767 and the
4,096-block held cohort are unchanged. The full 68-event Host scene includes
NoteOff and FX tail. The original 10-suite test and its negative result remain.

| Loop analysis | Output | Raw overload, held cohort | Raw overload, whole scene | Final hard clips, whole scene |
|---|---|---:|---:|---:|
| Off | Bypass | 9,915 | 9,950 | 9,950 |
| Off | Peak64 | 9,915 | 9,950 | 0 |
| On | Bypass | 4,445 | 4,479 | 4,479 |
| On | Peak64 | 4,445 | 4,479 | 0 |

Every row must observe 24 actual sample Voices; the on rows must observe 24
actual Loop Voices with the unchanged automatic 32-frame selection. Tests
repeat renders and compare complete PCM. New output statistics never relabel
the original bypass negative as clean audio. These are Host results, not
physical timing or listening evidence. Concurrent target timing need not
produce these exact serialized Host clip counts.

## Two independent MIDI channels

`LoopRealtimeSession<MaxZones, MaxPlans, Channels>` reserves exactly the
configured channel count. The qualification target uses `<16,8,2>`; the old
single-channel default specialization remains supported. Only channels 0/1
are accepted by this target. This is not a claim of 16-channel capacity or a
USB/UART/BLE input implementation.

Each channel has its own provider, optional plan handoff, lifecycle phase and
two immutable playback generations. The source, parser, PCM arena, directory,
allocator, VoicePool, FX and SPSC queue are shared. One control actor owns all
parsing and scratch; each control call acknowledges completed channels and
performs at most one round-robin Prepare. A READY generation cannot be reused
before the audio commit and control retirement acknowledgment. Active Voices
on the other channel retain their independent sample refs during a change.
All atomics/reference-bearing records remain INTERNAL. Only the same audited
scratch, immutable generations and private control allocator use PSRAM.

The original phrase `two-channel-phrase-v1` is defined once in
`firmware/app/LoopRealtimeReplay.h`, shared by Host and target. Two two-second
parts exercise chords, melody, NoteOff, independent sustain/pan/expression,
CC21/91/93, pitch bend, channel pressure and two channel-1 Program Changes.
Channel 0 holds a Loop Voice while channel 1 prepares and replaces its Program.
Each next part is scheduled after observing the preceding commit. Event offsets
within each part are fixed; async Prepare gaps can differ across runtimes.
A canonical byte hash identifies the score independently of C++ struct padding;
the target logs its actual part start frames for event-hash reconstruction.

The physical 24-Voice cohort runs separately, with channel 1 silent. Its extra
channel-1 AllSoundOff event does not reduce the original cohort's voices,
velocity, FX, observation length or timing threshold. The musical phrase is
not used to replace that sustained measurement.

The new QEMU replay fixture only widens the original synthetic SF2 key coverage;
it preserves the original PCM and loop geometries and has its own SHA-256:
`98c6ed2230abd112e073996c1a1747e47e4eef1e72c6894c2cb0ca4e6750431c`.
The old startup/analysis fixture and all schema-1 manifests remain unchanged.

## Verification and concentrated hardware acceptance

The first four-mode physical batch at `1aeb2cf` failed realtime qualification
on 2026-10-30. The complete logs establish correct image/source/asset identity
and actual on-mode Loop Voices, but stop after 2–3 held-cohort blocks because
rendering exhausts the unchanged DMA backlog window. All four real-bank raw
overload counters were still zero at that point; that incomplete prefix does
not reverse the negative Host results above or qualify Peak64's complete
physical workload. The 117-event replay was never reached. Preserve the
failure and the separate post-completion USB reconnect fragment as documented
in `benchmarks/V1_LOOP_REALTIME_PHYSICAL_2026-10-30.md`.

Run the complete Host suite and independent sanitizers, then the dedicated
workflow's **Loop on/off × output bypass/peak × QEMU/physical** build matrix.
The four QEMU profiles execute real Xtensa/FreeRTOS actors. Current QEMU runs
contain 6,144 input blocks; physical runs retain 32,768. Source identity, dirty
path evidence, compiled atomic backend, exact flash input hashes, event hashes,
output counters and execution JSON reconstruction all remain required.

```sh
python3 scripts/check_v1_voice_baselines.py
python3 scripts/check_v1_loop_startup_default.py
bash scripts/check_v1_output_bypass.sh
python3 -m unittest tests.test_v1_loop_realtime -v
bash scripts/test_v1_loop_realtime.sh
# Explicit, separate build directories; no command here flashes a board:
bash scripts/build_v1_loop_realtime.sh qemu on peak
bash scripts/build_v1_loop_realtime.sh physical on peak
```

Host artifacts include paired real-bank cohort and musical WAVs, their hashes
and latency metadata. Playback comparisons must account for the 64-frame delay
and use the same external playback volume. File generation/checksums do not
fill the audible worksheet. Keep the accepted 604/602/62 preferences; this
batch is not another Crossfade scoring exercise.

Each physical bundle contains a standalone `hardware.py`. `inspect` verifies
the full file list, SHA-256 values and four matching flash inputs offline.
`backup` requires an explicit serial port and saves all 16 MiB plus hash and
device MAC to a new directory. `flash` and `rollback` require that verified
backup and the same observed board MAC. There is no port scanning or automatic
flashing. Keep logs outside the immutable bundle. The existing layout warning
still applies: never write only the new app over the old M7 FAT partition map.

Run the preserved #26 timing baseline and new bypass/peak on/off candidates in
one coordinated board session. Record actual internal free/largest/minimum,
PSRAM reserve, task stack margins, Prepare and render distributions, maximum
and final backlog, I2S writes, actual Loop Voices, and all independent gates.
The strict 24-Voice cohort remains 1,451 us; a failed result requires honest
capacity characterization/optimization. It does not authorize reducing the
load and relabeling it 24-Voice PASS. The new heavy-DSP boundary, broader native
SF2 compatibility, physical listening, analog headroom and release/license
freeze remain separate V1 requirements. No board execution or V1 publication
is implied by build or QEMU success.
