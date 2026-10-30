# V1 concurrent control/audio integration (default OFF)

## Position and scope

The original integration described below merged in PR #26 as
`4b097e8bd65e86893e343943342b51e42377c03d`. Its final head passed 12 workflows /
24 checks; the merge's two main workflows also succeeded. This document keeps
the schema-1 single-channel workload and negative headroom evidence as history.
For the new two-channel replay and independently optional output candidate,
including schema-2 profiles, see `V1_OUTPUT_HEADROOM.md`. New profiles do not
retroactively qualify the old images or reinterpret their evidence.

PR #25 merged normally into `main` as
`51dc57d42057e9c23d3cf593717f56cc3841c92e`. Its reviewed head was
`5502e97468fdace04ed43180f43bf477179a541b`; all 17 triggered workflows and 25
check runs succeeded. Its serial startup and QEMU resource evidence remain
valid for that scope. The current batch adds a separate concurrent application;
it does not make `LoopStartupSession` safe for concurrent calls.

`GM_SYNTH_BUILD_LOOP_REALTIME_QUAL` defaults OFF. An explicit build selects the
new entry point and compiles the experimental Voice ABI uniformly. Both `on`
and `off` profiles compile that ABI; the mode controls automatic loop analysis.
The off build is not a compile-time-disabled object-size baseline.

The new profile provides one real control actor and one real audio actor,
timestamped MIDI events, Program/PCM/plan commit at audio block boundaries,
continuous render, and a dedicated Korvo I2S/DMA sink on physical hardware.
This is a bounded channel-0 integration and qualification route, not a manual
USB/UART/BLE MIDI frontend or a claim that all 16-channel musical workloads have
been requalified with optional loops enabled.

## Actor and lifetime contract

- Startup verifies the immutable mapped bank, resolves the initial program,
  loads PCM, analyzes eligible resident loops, and activates the initial group
  before either actor starts.
- The control actor alone parses, loads, analyzes, drains Program Change
  requests, prepares inactive storage, and acknowledges retirement. Its bank
  reader checkpoints after at most 32 KiB or 64 read calls; checkpoints may
  yield or cancel, never re-enter the session or change its resource.
- The audio actor alone commits READY groups, applies timestamped events,
  renders Voices/effects, and writes blocks to I2S. It never calls the parser,
  source reader, analyzer, or control-only diagnostics.
- The event queue has one producer. Enqueue does not inspect the audio actor's
  mutable frame counter. Live telemetry is copied through explicitly owned
  synchronization fields; it is not obtained by reading the other actor's
  non-atomic statistics.
- Stop is a request. Each actor acknowledges after its final graph mutation.
  The supervisor joins both tasks before graph, bank mapping, PCM arena or
  external storage destruction. A join timeout is a failure and does not
  authorize freeing potentially live resources.

The FreeRTOS join adapter waits for the actors' final resource-access completion
stores; the remaining instructions only notify a copied parent handle and
self-delete. The idle task may reclaim task stacks/TCBs later. The `stopped`
heap snapshot therefore does not claim that every RTOS allocation has already
returned to its pre-start value. A separate event producer must also quiesce
before destruction; the supplied target has control as its sole producer.

The transaction gate retains the existing order: control Preparing → READY;
audio commits sample snapshot, playback snapshot and optional plan together;
audio publishes Committed last; control releases retired plans and Program
pins only after that state. Old sounding Voices keep independent PCM refs.
READY storage is not reused while audio may still read it.

Internal memory placement remains explicit for the Session and its atomics,
publication directory, reference-bearing records, handoffs, Voices, queue and
FX. Only the specifically audited external storage fields may use PSRAM.
Those fields are one shared control scratch area, two immutable playback
generations, and the control-only allocator/hash/pin `SampleResidencyManager`.
The latter's phase-A leases are not the cross-core audio ownership path.
`ProgramPrepareController` and its actual published reference records remain
INTERNAL. The session destroys that internal graph, then its private external
manager, then the verified bank and PCM lease, in that order.
Actual `sizeof` values and the largest available internal block are reported
by each build/run. Successful compilation is not evidence that the board has
enough contiguous internal memory.

The inherited IDF 6.1 PSRAM atomic audit covers its documented four-byte
load/store backend and placement contract. It is not a claim that every
existing atomic statistic, reference acquisition, ISR operation or whole audio
path has a proven worst-case execution time.

## Pinned workloads: an enabled Voice must actually execute

| Property | QEMU | Physical Korvo N16R8 |
|---|---|---|
| Source | Existing deterministic synthetic native SF2 | Unchanged M7 FM/GM subset |
| Source bytes | 25,866 | 5,898,214 |
| SHA-256 | `9b8bfcf0226da056839739d65b12cc145aaa2488c26aa753a2ef8a5dea8987fd` | `9650eb1c2202e2d92aa3be155e4e895cf6531a241c8ea413fc09e7f9159d4a62` |
| Initial / replacement Program | 0 / 1 | 16 / 1 |
| PCM arena | 20,480 bytes, internal | 4,718,592 bytes, PSRAM |
| Scheduled blocks | 2,048 | 32,768 |
| Output | Concurrent render; no I2S/DAC | 44.1 kHz, 64 frames, 16-bit mono average to ES8311 |
| Analysis modes | Explicit on and off | Explicit on and off |

The old startup's physical Programs 0 and 1 have `loop_mode=none`. Reusing
those notes would exercise an enabled build without ever rendering a loop
plan. The new physical workload starts on Program 16 (`Drawbar Organ`), changes
to Program 1, then returns to Program 16. Selection still uses the frozen
automatic analyzer; the runtime contains no Sample-ID allowlist or forced fade.

A fresh inspection with the frozen production Prepare exporter and unchanged
`prepared-loop-review-v3-source-matched-peak` policy found 67 prepared Programs
and 67 distinct geometries in the existing FM subset: 28 context-blocked,
one Direct/no-benefit, one Direct/review and 37 Host candidates. Program 16's
one Zone is mono, 44.1 kHz, continuous, with no NoteOn address modulation.
Its effective source-coordinate loop is `[663800, 679982)` (16,182 frames),
within source range `[659755, 679982)`; the frozen policy selects 32 source
frames. `Sample 10 / FM016` is an audit locator only. This is workload coverage,
not a scoring change or listening approval. Reproduce the inventory with:

```sh
V1_LOOP_BUILD_DIR=build/v1-loop-realtime-inspect bash scripts/build_v1_loop_exporter.sh
python3 -m tools.v1_effective_loop_survey assets/sf2/subsets/FM_GM_Open_subset_6MiB.sf2 \
  --exporter build/v1-loop-realtime-inspect/export_prepared_loops --limit 4096 \
  --output build/v1-loop-realtime-inspect/fm-effective.json
```

The on-mode execution gate requires at least one actual loop-enabled Voice;
off-mode requires zero. A configuration flag, a nonzero plan count or a
successful compile cannot substitute for that observation. The synthetic
fixture also exercises UntilRelease/NoteOff semantics; the physical FM organ
is a continuous-loop workload and does not by itself qualify every release mode.

## Verification and evidence boundaries

The dedicated workflow runs Host ownership/concurrency tests, ASan/UBSan, a
separate GCC ThreadSanitizer run,
configuration/manifest/evidence rejection tests, and four target builds:
QEMU/on, QEMU/off, physical/on and physical/off. QEMU executes real FreeRTOS
actors and checks MIDI coverage, overlap, runtime health and complete shutdown.
Its timing and heap observations are diagnostic emulator data.

Host and QEMU concurrent scheduling need not produce identical PCM hashes or
the same block for an asynchronously prepared Program Change. Required
invariants and event counts are checked directly. A whole-log comparison must
not accidentally treat nondeterministic scheduling as a failed DSP regression,
or use a changed checksum as a substitute for ownership checks.

```sh
python3 -m unittest tests.test_v1_loop_realtime -v
bash scripts/test_v1_loop_realtime.sh
CXX=clang++ SANITIZE=1 V1_REALTIME_DIR=build/v1-loop-realtime-asan \
  bash scripts/test_v1_loop_realtime.sh
THREAD_SANITIZE=1 V1_REALTIME_DIR=build/v1-loop-realtime-tsan \
  bash scripts/test_v1_loop_realtime.sh

# ESP-IDF v6.1, independent directories, no flashing:
bash scripts/build_v1_loop_realtime.sh qemu on
bash scripts/build_v1_loop_realtime.sh qemu off
bash scripts/build_v1_loop_realtime.sh physical on
bash scripts/build_v1_loop_realtime.sh physical off
```

The realtime build command can be repeated in the same checkout, including
after a failed or successful build. After checking ESP-IDF v6.1, it moves the
complete previous attempt to a unique directory under
`build/v1-loop-realtime-history/<profile>-<on|off>-<bypass|peak>.XXXXXXXX/build/`
and prints that path. It then rebuilds at the original output path with the
current pinned defaults and a new sdkconfig. Old bundles, logs, configuration
and execution evidence retain their original bytes in the archive; other
mode combinations are unaffected. A failed retry returns its actual error and
does not leave the previous bundle or execution result in the current path.
Archives are records, not directories to resume with CMake. Retain the ones
used for hardware results; archive cleanup is left to the operator.

If recording shell output with `tee`, write it outside this command's output
directory so that opening the log cannot truncate the previous attempt's log
before the script archives it. Only run one build of a given combination at a
time. The QEMU runner additionally
needs Linux `timeout`, the IDF QEMU tool and the matching Host log downloaded
by CI; it is not presented as an unchanged macOS runner.

The real-board test records memory at allocation and shutdown, source hash and
Prepare/read/analyze timing, continuous renderer latency, backlog/DMA margin,
queue/write faults, actual loop-enabled Voice counts and explicit measured
cohorts. Individual transient blocks over 1,451 us remain distinct from a
sustained polyphony boundary or an unrecovered DMA backlog. Measured failing
cohorts must remain visible; do not reduce the requested Voice count, increase
buffers or suppress watchdogs to turn them into passes.

The historical `bb31908b` / actual `bb31908-dirty` three-run 24 normal/full-FX
and eight heavy-DSP Voice evidence remains accepted. It is not proof of the
new on-mode path's polyphony. New physical capacity/timing, representative
listening, analog headroom and V1 release approval remain separate gates.

### Known headroom failure in the new physical workload

The exact pinned FM Program 16, 24 simultaneous sample Voices on keys 48–71,
velocity 64 and CC91=40 / CC93=20 cohort produces nonzero digital clipping in
the Host renderer in both modes. The 4,096-block held-note cohort recorded
9,915 / 4,445 clipped samples for direct/on; after the remaining NoteOff events
and 256 blocks of FX/release tail, the complete 68-event scene recorded
9,950 / 4,479. These are Host measurements with serialized control service,
not predicted exact Xtensa counts. It correctly maintains 24 active Voices and, in on mode, 24 genuinely
loop-enabled Voices with the automatic 32-frame choice.

This is a known output-headroom failure, shared by both modes, not a concurrency
or sanitizer failure and not a physical timing result. The workload, velocity,
master, DAC level and clipping gate remain intact. The regression case checks
real execution and correct rejection of the failing headroom result; passing
that negative test does not turn the cohort into a clean-audio PASS. The target
prints cohort/global clipped-sample counts and keeps its zero-clipping physical
gate. The current physical profile is therefore expected to fail headroom while
still yielding memory, latency and actual 24-Voice execution measurements.
Resolve and independently verify output headroom before presenting the new path
as a qualified 24-Voice product configuration. No threshold or volume reduction
is used to hide this result.

## One consolidated physical handoff

Each target artifact contains `flash-bundle/` with a relocatable `flash_args`,
all four actual flash inputs, ELF/map, sdkconfig, generated asset manifest,
compile database, build/atomic audits, source commit/tree and dirty state, and
SHA256SUMS. The physical bank's existing source/license notices are included.
Packaging is not a product redistribution-license approval or V1 publication.

`source-status.txt` preserves dirty paths; `source-identity.json` compares actual
build inputs with HEAD. A dirty checkout can still have unchanged tracked inputs.
Untracked files contribute only path, size and hashes; their contents are not archived.

Use the exact matching on/off bundles after the batch's automated gates finish.
Keep 44.1 kHz, 64-frame blocks, 8×64 DMA, master Q15 32767 and DAC −12 dB.
The new `loop_bank` raw partition layout differs from the historical M7 FAT
layout. Flash the whole matching image set, never just the application binary
over an old layout.

Before the first transition, keep the current full 16 MiB board image and its
hash so the accepted historical configuration can be restored. With the
ESP-IDF 6.1 esptool environment and the actual connected port:

```sh
python -m esptool --chip esp32s3 --port PORT read-flash 0 0x1000000 m8-before-realtime.bin
```

From the selected bundle directory, verify `SHA256SUMS` and use its complete
arguments:

```sh
python -m esptool --chip esp32s3 --port PORT write-flash @flash_args
```

Retain the full boot-to-stop monitor log with the corresponding bundle manifest.
Return on and off logs together; no new subjective PASS is inferred from them.
Rollback uses the saved full image at address zero, preserving its original
flash configuration. The backup is device-specific and may contain the board's
existing NVS settings; retain it with the original qualification artifacts.
Official command references: [flashing firmware](https://docs.espressif.com/projects/esptool/en/latest/esp32s3/esptool/flashing-firmware.html)
and [basic commands](https://docs.espressif.com/projects/esptool/en/latest/esp32s3/esptool/basic-commands.html).

The prior GeneralUser 604/32f listening preference remains recorded; 602/62 did
not receive a forced variant preference. New original-instrument listening
still uses `V1_AUDIBLE_ACCEPTANCE.md`; waveform generation or checksums cannot
fill unobserved rows with PASS. The current mono speaker route cannot qualify
spatial stereo. A DAC −6 dB experiment needs its own clearly identified image
and analog-headroom observations.
