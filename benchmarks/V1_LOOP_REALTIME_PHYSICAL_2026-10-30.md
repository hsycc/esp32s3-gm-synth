# V1 Loop Realtime Physical Failure — 2026-10-30

**Disposition: all four complete primary captures fail the new physical
qualification.** The user successfully built, inspected, flashed and ran the
four Loop on/off × output peak/bypass combinations. The 24-sample-Voice cohort
stops after only two or three blocks because producer work exceeds the realtime
budget. The musical replay never starts. These are retained negative hardware
observations, not a physical-capacity, headroom, listening or release approval.

Machine-readable measurements, every recorded `REALTIME_*` field, input hashes
and the separate reconnect segment are in
[`V1_LOOP_REALTIME_PHYSICAL_2026-10-30.json`](V1_LOOP_REALTIME_PHYSICAL_2026-10-30.json).
That file is an observation report, not `execution.json` or a successful
qualification certificate. `physical_execution=true`; `qualified`,
`runtime_approved`, `physical_timing_approved`, `listening_approved` and
`release_approved` remain false.

## Identity and Flash evidence

All four uploaded inspection reports identify:

- Source commit: `1aeb2cf61df48f180f539ff560fd41d768179903`.
- Source tree: `37d914a56261a2beb26c1431ed480012e2ef6d8d`.
- Clean source and all three actual-build-input identity checks true.
- `profile=physical`; matching on/off, peak/bypass and 0/64-frame output latency.
- The exact same configuration object across all four builds.

Each inspection report is equal to the final schema-2 report in its previously
supplied local build log. All primary runtime captures print `App version:
1aeb2cf`, ESP-IDF v6.1 and the expected mode. The native SF2 is 5,898,214 bytes
with SHA-256
`9650eb1c2202e2d92aa3be155e4e895cf6531a241c8ea413fc09e7f9159d4a62`.
The reported runtime asset hash matches the bundle in every case.

Fixed conditions remain ESP32-S3 N16R8, 240 MHz, Flash DIO 80 MHz, octal PSRAM
80 MHz, 44.1 kHz, 64-frame blocks, DMA 8 × 64, master Q15 32767, ES8311 −12 dB,
mono average of the two rendered channels, and a 4,718,592-byte PCM arena.
Assertions, the 5-second panic WDT with both idle tasks monitored, and the
PSRAM atomic workaround remain enabled.

Every Flash log records the same board and four successful writes, each followed
by `Hash of data verified.`: bootloader at `0x000000`, partition table at
`0x008000`, app at `0x010000`, native SF2 at `0x210000`. Those verification lines
are 45, 53, 76 and 417 in each 419-line log. There are no reported Flash errors.
The machine-readable report retains each image size and manifest SHA-256.

The reviewer independently hashed the uploaded reports/logs and compared their
identities. Actual binaries and board Flash readback were not supplied to the
reviewer; write verification is the esptool evidence. Developer-machine paths,
serial-port names and the board identifier are omitted from this report.

## First complete capture from each combination

The required workload is 32,768 input blocks, including a 4,096-block cohort
with exactly 24 sample Voices. In on mode all 24 must execute LoopVoice. The
strict cohort maximum is 1,451 µs per 64-frame block; the configured DMA window
is 11,610 µs.

| Metric | off-bypass | off-peak | on-bypass | on-peak |
|---|---:|---:|---:|---:|
| Input blocks / required 32768 | 428 | 428 | 876 | 890 |
| Input frames | 27392 | 27392 | 56064 | 56960 |
| Cohort blocks / required 4096 | **3** | **3** | **2** | **2** |
| Cohort sample Voices min/max | 24/24 | 24/24 | 24/24 | 24/24 |
| Cohort LoopVoices min/max | 0/0 | 0/0 | 24/24 | 24/24 |
| Cohort render average, µs | **4188** | **4280** | **5152** | **5265** |
| Cohort render maximum, µs | **4498** | **4603** | **5201** | **5315** |
| Global render average, µs | 651 | 733 | 442 | 532 |
| Global render maximum, µs | 5055 | 5315 | 6111 | 6004 |
| Global maximum/final backlog, µs | **12121** | **12712** | **12334** | **12498** |
| Cohort maximum/final backlog, µs | 8424 | 8746 | 7587 | 7836 |
| DMA margin minimum, µs | 0 | 0 | 0 | 0 |
| Backlog exceeded window | 1 | 1 | 1 | 1 |
| Active I2S send queue overflows | 2 | 2 | 2 | 2 |
| Events applied / queued | 45/69 | 45/69 | 45/69 | 45/69 |
| Program switches / Prepare successes / acks | 2/2/2 | 2/2/2 | 2/2/2 | 2/2/2 |
| Musical replay events / complete | 0/0 | 0/0 | 0/0 | 0/0 |
| Integration / cohort / replay | FAIL / FAIL / FAIL | FAIL / FAIL / FAIL | FAIL / FAIL / FAIL | FAIL / FAIL / FAIL |

In each primary capture, the boot identity is at lines 59–62, mode/asset at
78–82, gates at 88, result/output/replay/control at 89–92, timing at 93–94,
health at 95 and cohort at 96. `REALTIME_FAIL reason=qualification` is at line
101, followed by `Returned from app_main()` at 102. The complete failure is
present; this is not a missing end-of-log inference.

The observed failure sequence is:

1. The first measured 24-Voice cohort blocks take approximately 4.2–5.3 ms on
   average, above the 1.451 ms budget.
2. Global producer-work backlog exceeds the 11.610 ms window. The active I2S
   measurement also records two send queue overflows.
3. The bounded failure path stops the audio/control actors before the required
   workload, sustained cohort and musical replay complete.
4. Resource cleanup completes, and the application reports qualification FAIL.

`backlog` is software producer-work accounting. `dma_queue_overflows` is the
I2S `on_send_q_ovf` callback count. These observations do not measure an exact
analog dropout length. `write_errors`, `timeouts`, `partial` and
`event_queue_drops` are zero, but those zeros do not override the backlog and
queue-overflow failures. Pending events at an early stop are not by themselves
proof of event loss.

Only two or three cohort blocks were captured. This establishes an early
24-Voice render-time failure, **not a steady-state capacity boundary**. The
global averages mix low-load blocks and differing Prepare durations; they
must not be used to conclude that on mode is faster than off mode. The
aggregate telemetry does not isolate the underlying DSP stage or prove the
implementation cause.

## Output/headroom boundary

All four prefixes report raw peak 15350, zero raw overload, zero hard clipping,
zero limited frames and unity gain. `raw_headroom=PASS` and
`digital_headroom=PASS` therefore describe only these short rendered prefixes.
The fixed overloaded workload was not completed. These lines neither overturn
the retained Host clipping evidence nor qualify real-bank output headroom.

The target's synthetic extreme-value output probe does pass: 80 bypass clips,
zero peak clips, peak output 32766, first block zero, 64 input / 128 output
frames. Retain that bounded probe result separately from musical acceptance.

Both peak runs stop with `drain_frames=0`. The implementation emits its pending
64-frame output block only after normal full-workload completion. Missing drain
on this early failure is not independent evidence of a broken normal drain.
The replay counters are all zero, so the intended 117-event two-channel music
has not been heard or timed by this test. No subjective listening notes were
supplied with the files.

## Memory and controlled stop

All complete runs report zero PCM allocation failures and stale references,
with PCM high water 168,348 bytes. Audio stack remaining is 5,400–5,592 bytes;
control stack remaining is 10,580 bytes. No WDT, panic, abort or assertion
failure is present in the supplied captures.

The stop record reports audio/control stopped and release complete, with
`mapped=0 arena_live=0 live_refs=0 acquirers=0 stale_refs=0`. Free PSRAM returns
from 3,563,108 bytes while ready/joined to its original 8,386,156 bytes.

Internal free memory is 375,820 bytes before, 199,532 ready, 199,396 joined and
375,100 stopped, with a 173,048-byte low-water mark. The final value is 720 bytes
below the before value. Its ownership/cause is not established by these logs;
do not claim either a proven leak or complete internal-heap recovery. These
short runs are not soak or long-run reliability evidence.

## Separate on-peak reconnect segment

`on-peak-run.log` contains one complete primary capture through line 102. At
103 the monitor reports `read failed: [Errno 6] Device not configured`; 104 waits
for reconnect. Lines 105–126 contain another partial segment beginning at
`REALTIME_READY`, without its own boot identity, `REALTIME_BEGIN`, output probe
or before-memory record. Preserve it separately; do not merge it into the
first run or count it as a fifth independently identified complete capture.

The partial segment also reports 890 blocks, two cohort blocks, 24 LoopVoices,
two send queue overflows, no replay events and qualification FAIL. Its global
render average/max is 530/6011 µs, backlog 12506 µs, cohort maximum 5305 µs,
cohort backlog 7839 µs and audio stack 5448 bytes. The first complete segment
has slightly different timing/stack values, so this is not an identical text
duplication. Its missing startup records limit independent attribution.

The USB disconnect occurs after the primary segment already reports failure
and returns from `app_main`; it does not explain that earlier failure.

## Historical baseline and next gate

The accepted [`bb31908b` / `bb31908-dirty` baseline](M8_PHYSICAL_THREE_RUN_2026-10-24.md)
and the user's explicit acceptance of dirty remain unchanged. This report does
not requalify or invalidate that earlier path, and the older 24-normal / 8-heavy
results do not qualify this new path.

Keep the present workload, gain, buffer, WDT and 24-Voice gates fixed while
investigating the new path. Preserve the failed evidence. After implementation
and CI/QEMU verification, a consolidated physical rerun must complete the
cohort, replay, output accounting and health gates before capacity/headroom
claims advance. Hardware timing and listening remain separate release gates.

## Exact uploaded input hashes

Original attachments are retained by the user. Raw logs, local paths, board
identifiers and firmware binaries are not copied into this repository.
SHA-256 values below cover the exact uploaded bytes, including line endings.

| Input filename | Bytes | SHA-256 |
|---|---:|---|
| `off-bypass-bundle.json` | 2678 | `6d19e105285ccc898284571a73d81a7e0c994e9c937b691e4c463fcf9caff05e` |
| `off-bypass-flash.log` | 41919 | `080d965ded7ab0e805f76df56990e233e7164b3207b5c32746eeaf1ef6a22b56` |
| `off-bypass-run.log` | 8125 | `94c6c3883a0621d171c77b405f84fcd6aa5fd5296e035fb26b042435f2ed70b7` |
| `off-peak-bundle.json` | 2677 | `c9ea90b69b8e9c3c4edc6fe8aa90049b580d17df6d6ed47b17e46ff3bc904ea6` |
| `off-peak-flash.log` | 41911 | `4426ddbde00383f452e098365102e245c1ae70100384680aba39d317ac2a4365` |
| `off-peak-run.log` | 8124 | `d23e9c133fbc0e3c7600d168bd9061737faf96bab8d298c31463d4c9863b50e8` |
| `on-bypass-bundle.json` | 2677 | `c412c6d74d91b1a27caaa8d411f65970e914fddf03cd0c72c9368c88f729d100` |
| `on-bypass-flash.log` | 41916 | `a6d3d9a1d3306fe0721be11c5027b2be17faa9419dcd5ab00bf84c39f1d53929` |
| `on-bypass-run.log` | 8140 | `283a6c20c0f7d04abc9af8bf26e1ae25638a2ddc0b88ee32ff8cdd05a69c39a3` |
| `on-peak-bundle.json` | 2676 | `e39d4fe738c4eca44ee44e34ef6bd682230a7431f2ee2e4a43f7acf783b83b7d` |
| `on-peak-flash.log` | 41907 | `7dcab84f803a1fba1c31bd6bc52d55fcd92d9afa511bb7106c2498d3ad27f4ce` |
| `on-peak-run.log` | 12193 | `e7a2e648f33ead63dfffa698ab9c3cb6f890c0d43a150880249cc67c249bb64d` |
