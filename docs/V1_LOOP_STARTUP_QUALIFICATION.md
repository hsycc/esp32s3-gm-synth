# V1 owned startup qualification (default OFF)

Stacked on d72dcbbd / PR #24. This does not enable the product by default and is
not an I2S/DMA, listening, concurrency or 24-Voice performance acceptance.

## Ownership

`LoopStartupSession` owns a move-only `StartupLease`, verified reader, and entire
private Program/plan/Voice graph. No PCM/snapshot/plan pointer escapes. Start is
one-shot. A second start returns AlreadyUsed and releases only its new lease.
A boot-local serialized `StartupSerials` allocator burns IDs on failure and never
wraps; it is not persistent identity across reboot. Bank generation is group-local.

Stop must run in the same serialized task (or after joining every task). It resets
Voices, removes the callback, clears plans, verifies zero directory references,
and destroys the whole graph before destroying the reader and releasing the
arena/source lease. Remaining Active/READY snapshots and their Program pins are
discarded only as part of this stopped whole-group destruction. This is NOT an
independent controller cancel/retire API. A reference cleanup failure holds the
resource group; destruction then aborts rather than silently unmapping live PCM.
Backend context must outlive its lease. A const pointer alone is not immutable
storage; caller must supply exclusive, pinned storage. Invalid overlapping or
misaligned source/arena spans are rejected. Serial/rebind checks are not signatures.

## Root entry and profiles

`GM_SYNTH_BUILD_LOOP_STARTUP_QUAL=OFF` is the CMake default. When explicitly ON,
the root main component selects the qualification `app_main` in
`LoopStartupQualification.cpp` instead of the unchanged legacy `app_main.cpp`. It compiles the same optional Voice ABI
throughout the project. The original main body, SDK defaults, PCM assets, scoring,
transaction implementation and DSP remain unchanged.

Build only, never auto-flash:

```sh
bash scripts/build_v1_loop_startup.sh qemu on
bash scripts/build_v1_loop_startup.sh qemu off
bash scripts/build_v1_loop_startup.sh physical on
```

Each uses a separate fresh build/config/staging directory. The script refuses to
reuse a configured directory; it never deletes the user's default/M7 build. The
raw `loop_bank` read-only partition is at 0x210000, size 6 MiB, in a NEW 16 MiB
partition table. Flashing this profile replaces the M7 data layout. Back up the
existing flash and use the full matching generated flash_args when qualification
is eventually approved. Do not use app-only flashing against the old FAT image.

The target maps the exact manifest length using ESP-IDF v6.1
`ESP_PARTITION_MMAP_BLOCKS_WRITE`, requires a read-only partition and validates
SHA-256 of actual mapped bytes. SHA verification yields per 64 KiB. This profile
starts no OTA/storage writer. It is not a generic filesystem/SD hot-swap owner.

QEMU uses the pinned 25,866-byte synthetic SF2 and 20 KiB internal PCM arena.
Physical build uses the unchanged 5,898,214-byte M7 subset and **4.5 MiB PSRAM PCM
arena**, not the 32 MiB full GeneralUser. Session metadata is separately allocated
in INTERNAL memory and logged. Allocation failure is a failure, never a silent
reduction of zones, arena or feature load. Workspaces support 32 Zones; optional
plan capacity 8; these are not polyphony settings. Excess Program size fails
Prepare. Analysis allowance remains the frozen provider default (8/400000).

## Exercise and evidence

A serialized one-channel diagnostic drives 256 x 64 frames. NoteOn at frame0,
Program Change in block128, new NoteOn in block129; old tails remain pinned across
the change. Analysis counters cannot increase during rendering. Initial automatic
fade must be active in QEMU-on; QEMU-off must stay Direct. Frozen native output
trace hashes are respectively 2034267099694268986 and 14037194901542623014.
Raw synthetic output hash includes PCM and active-Voice count; it is not an
independent proof of all original-instrument DSP or audible quality.

Logs report startup including hash/parse/load/analysis, hash alone, maximum service
call, average/max pure render, current/peak PCM occupancy, internal heap and source
release/unmap status. Timing includes instrumentation, and service runs BETWEEN
blocks, not simultaneously with deadline-driven audio. No audio sink is installed;
there is no audible playback or I2S Hard Gate in this profile. QEMU times do NOT
represent physical latency. Real hardware timing, memory fit, I2S scheduling,
concurrent Prepare/render and original-instrument listening remain gates.

Native tests cover both modes, digest/framing/hash/space/preset/serial failures,
rebind rejection, live-tail destruction, READY/unacknowledged-commit stop, budget
and cancellation fallbacks, idempotent stop and repeated new sessions. Clang
ASan/UBSan executes actual freeing of both input bank and PCM arena. Source guard
checks the unchanged legacy entry directly.
This is a source-equivalence claim, not target binary equivalence.

CI status must be reported separately from implementation. Physical profile is
compile-only in CI; no physical execution is claimed by this document.
