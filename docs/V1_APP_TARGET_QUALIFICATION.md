# Application transaction on Xtensa (default-off qualification)

Status update, 2026-10-28: PRs #19–#25 have merged into main. The scope below
records the original application-target batch. Feature defaults remain OFF;
current concurrent audio and hardware gates are tracked in
`V1_LOOP_REALTIME_INTEGRATION.md` and `V1_RELEASE_CLOSEOUT.md`.

This batch extends PR #23 at frozen `7c113d582221ac739f8adeef6428ad2a25d06363`.
It adds a standalone test application, not normal startup enablement. No changes
to production Voice, resolver, transaction protocol, Host policy, PCM bank,
normal CMake/default configs, DMA, gain or polyphony were part of that batch.
A submitted workflow is NOT evidence that target execution passed.

## Loader identity: actual bytes, constrained lifetime

`app::VerifiedReadOnlyBank` checks bounded length, RIFF/sfbk framing, a computed
SHA-256 and a trusted expected digest BEFORE exposing a reader or asset stamp.
Null bytes, zero session/generation, digest mismatch and hash failure leave it
unopened. Once open, it rejects rebind, including a wrong new digest, without
changing the existing session. Offset/length checks are overflow-safe. The
source and parser both read this exact verified span.

This is an immutable MEMORY-span adapter, not a general filesystem authenticator.
Caller guarantees immutable pinned storage for all reader/parser lifetimes.
The target test supplies generated `inline constexpr` bytes in linked read-only
flash, not a mutable borrowed buffer. The test mutation cases use isolated
buffers BEFORE opening. There is no file watcher, SD/OTA locking, signature,
anti-rollback, or concurrent open/reset. Expected digest and non-reused session
come from the trusted owner. SHA alone does not authenticate an attacker-controlled
manifest. HashFn is trusted platform code; the test uses OpenSSL on Host and PSA
Crypto SHA-256 on ESP-IDF v6.1, both compared with Python hashlib-generated data.

## Same production application on native and target

The frozen synthetic SF2 is 4,752 bytes, SHA-256
`e1c3af71233c2e6b6151e53dbca19ce7bd05e0f14d160cf3ed11a2da419af51f`.
It is the earlier application fixture, not GeneralUser. The test constructs real
parser/materializer/source/residency/control service, mailbox/router/resolver,
VoicePool, event queue/scheduler and LoopProgramRuntime/Channel. Two immutable
workspaces (4 zones each) and two optional plans are used; these are NOT polyphony
limits. Large storage is static test RAM, not the FreeRTOS task stack. PCM arena
is 16 KiB for these tiny samples, not a 4.5 MiB product-memory qualification.

Three suites:
1. Nineteen identity/framing/range/failure assertions, including altered bytes,
   failed hashing and a forbidden rebind after successful open.
2. A deterministic 1,024-frame, 20-event script uses actual MIDI event routing
   and scheduled block boundaries: NoteOn, NoteOff, CC64 sustain, CC7, pitch bend,
   CC120, Program Change, same-Sample different-Zone selection, fixedVelocity,
   capacity Direct fallback, plan rejection, unavailable Program, old Voice
   retention after retire, and final reclamation. Four joint commits include
   initialization. Nonzero Crossfade is explicitly asserted. No DSP normalizing
   patch modifies the fixture; it uses its generated instrument parameters.
3. A real worker thread / FreeRTOS task pauses a provider after BOTH production
   exchanges are READY: 500 polls must keep the old pair active. Then 32 two-task
   prepare/commit/start/render/ack cycles run. This second stress segment submits
   control mailboxes, not MIDI PC bytes; the preceding script tests MIDI PC events.
   It is stress coverage, not TSan/formal race proof. On ESP the worker is pinned
   to CPU1 and the main task remains the counterpart; both yield.

Checks use explicit failure branches, not disabled NDEBUG assertions. Trace
hash/counts are anchored and compared native vs target. Native GCC and Clang
ASan/UBSan must agree. SHA identity, normal app return, completion markers, and
32-bit layout are required; missing/duplicate/out-of-order markers and fatal
lines even AFTER PASS reject the log. Target compilation uses the existing
strict compile database/response-file/ELF auditor. All project consumers must
compile with macro=1; no normal application flag is changed.

The independent image enables task WDT (5s, panic, both idle tasks), uses no
obsolete QEMU watchdog override, and disables SHA hardware acceleration only
in THIS test config to avoid depending on peripheral emulation. Neither
watchdog policy nor crypto profile is changed for production images. The
harness has no PSRAM/I2S/DAC/speaker; QEMU performance is not physical timing.

## Reproduction and evidence

- `bash scripts/test_v1_loop_app_target.sh` (OpenSSL headers/library required)
- `python3 -m unittest discover -s tests -p test_v1_loop_app_target_evidence.py -v`
- With ESP-IDF v6.1: `bash scripts/build_v1_loop_app_target.sh`, then
  `bash scripts/run_v1_loop_app_target_qemu.sh`.

Artifacts retain logs, configs, generated fixture, compile/response audits and
maps. Original application/Voice/full-bank/default-IDF workflows still apply.
The new wrapper has target execution evidence ONLY after this workflow passes.
Normal startup, mutable-storage source pinning/session issuance, bounded on-device
analysis/admission, stopped fault recovery, real-bank target coverage, actual
memory/CPU/I2S/DMA and listening remain separate gates. runtime_approved=false.
