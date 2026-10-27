# Application-facing loop orchestration (default off)

## Scope and explicit boundaries

Stacked on frozen PR #22 at `7a019caf209d07f500fdb81b22c0b4ff4d201f76`.
This batch connects the **production MIDI resolver and VoicePool** to optional
per-Zone plans and supplies one application-facing control/audio transaction
owner. It does not enable or instantiate that owner in the normal firmware
startup. There is no new on-device scorer, automatic JSON enable map, original
bank listening approval, or hardware timing/memory qualification. The compile
switch and root CMake option remain OFF by default. No main/SDKCONFIG changes.

`app::LoopProgramRuntime<N>` replaces the pair-only application block wrapper
for explicitly attached channels. `LoopProgramChannel<N>` owns fixed-capacity
plan buffers, a copied immutable asset/session stamp and one transaction gate.
Callers allocate a channel only where needed; the router stores 16 pointers,
not 16 full plan banks. N is a plan capacity, not a Voice/polyphony limit.

## Lifecycle and ordering

Exactly one serialized control producer and one audio consumer are required.
All service, commit, NoteOn selection and acknowledge operations for attached
channels must use this owner. Installing/removing its event callback, attaching
channels, reset and destruction are quiescent operations. Concurrent external
pair-only commits, cancellation or workspace mutation violate the contract.

1. Control changes Idle to Preparing BEFORE calling the production
   ProgramChangeControlService. The latter may publish sample and playback
   READY, but audio cannot commit them until the optional sidecar is complete.
2. ContextBuilder derives current identities from the actual source descriptor,
   residency, publication and effective prepared Zone. An optional control-only
   provider proposes bounded parameters; each proposal is independently checked.
   Inapplicable/rejected/declined/capacity-skipped entries remain Direct.
3. Only after the sidecar is fully published does control release-store Ready.
4. Audio acquires Ready at a block boundary. It preflights the exact sample,
   playback and sidecar pointers/metadata BEFORE altering any active pair.
   It commits sample/playback first and sidecar second, then LAST release-stores
   Committed. No events or rendering interleave within this single-consumer step.
5. Control acquires Committed, acknowledges/clears the retired sidecar first,
   then acknowledges the production pair and returns retired workspace ownership.
   Seeing production commit tokens alone is deliberately insufficient to retire.
6. Old Voices keep their independent existing sample references. After all old
   plans/Program pins retire, old tails still render; only the final Voice release
   makes the corresponding PCM reclaimable.

This is a protocol-level transaction for its cooperating tasks, not a promise
of a hardware atomic write across three independent exchanges. An impossible
invariant failure enters Fault, prevents further commits/new NoteOns and leaves
existing Voices renderable. Fault recovery requires stopped group teardown or
rebuild. `resetQuiescent()` releases sidecar pins; it DOES NOT secretly cancel or
acknowledge external READY snapshots. Production exchanges have no rollback API.

## Actual NoteOn selection, not a Sample-ID map

Both production resolver overloads retain the exact prepared Zone index in the
resolved batch when compiled in. The optional event callback receives that index,
the immutable active Program and the ORIGINAL MIDI key/velocity, together with
the final VoiceStartParams. This matters because SF2 fixedVelocity changes the
final voice velocity after raw-velocity Zone selection. Matching a plan against
the transformed velocity can incorrectly reject a perfectly valid Zone.

The handoff checks original event membership and binds by Zone index; two Zones
sharing a Sample ID can have different loops. Missing plans start Direct. A
matched plan calls production VoicePool.start exactly once. Pool-full/admission
or invalid-plan failure is NOT retried as Direct, avoiding duplicate statistics
and unintended stealing/admission side effects. Continuous/UntilRelease, sustain,
CC120, exclusive class, effects and voice ownership remain production semantics.

No source I/O, waveform analysis or dynamic allocation is introduced by these
new audio-side hooks. Existing production atomics/VoicePool work is unchanged;
this is not a measured worst-case S3 execution-time claim.

## Provisioning and startup obligations

The provider is intentionally an explicit experiment interface, not an approved
product policy. It runs on the control task, must not throw and must finish in
bounded work. Null/decline creates an empty optional sidecar without preventing
a valid Program Change. Once capacity N is reached, remaining Zones are skipped
in deterministic prepared order, not ranked by a new quality heuristic.

The trusted loader must digest and pin an immutable source and provide a unique
session and bank generation. Comparing caller stamps does not authenticate the
asset. Changing asset/session requires stopped reset and new channel ownership.
The present test stamp is clearly synthetic. Real startup digest provisioning,
streaming on-device analysis/admission and choosing which channels to attach are
NOT implemented by this batch. No future caller may reuse snapshot storage before
the joint acknowledgement, or destroy PCM/directory owners while Voices remain.

Typical explicit harness wiring:

```cpp
// One control owner; source/assets/workspaces/directory outlive this group.
LoopProgramRuntime<> runtime(control, samples, playback);
LoopProgramChannel<> channel(0, source, residency, directory, samples,
                             playback, immutable_asset, provider, provider_context);
runtime.attach(channel);              // quiescent
runtime.install(program_events);      // quiescent, before MIDI/audio starts
channel.initializeActive();           // control: initial optional sidecar
// audio: runtime.render(scheduled_renderer, frame_start, stereo);
// control: runtime.drainRealtimeRequests(); runtime.serviceChannel(0, workspace);
// control: runtime.acknowledgeCommitted(0); recycle only its returned workspace.
```

The initial sidecar also requires joint commit + control acknowledgement before
servicing another change. Calls on unattached channels retain the old pair path.

## Reproducible checks

`bash scripts/test_v1_loop_app.sh` generates a tiny deterministic synthetic SF2
and executes the actual parser/materializer/source/residency/control/mailbox/
router/resolver/VoicePool/scheduler. No reference-count mock replaces production.
The checked test requirements remain active with NDEBUG.

Eight suites cover exact Zone/fixed-velocity selection and capacity; single pool
start on full admission; joint tokens/retirement and old-tail reclamation; invalid
or declined proposals and empty sidecars; a paused provider AFTER both exchanges
are READY (500 audio polls must not commit); latest-request coalescing and missing
programs; commit-before-timestamp-zero NoteOn through the real scheduler; deliberate
non-racing READY corruption; and 1,000 concurrent service/commit/start/ack cycles.
Synthetic SF2 SHA-256:
`e1c3af71233c2e6b6151e53dbca19ce7bd05e0f14d160cf3ed11a2da419af51f`.

`bash scripts/check_v1_app_disabled.sh` rebuilds frozen 7a019caf and compares two
native default-off GM object files byte-for-byte. The old/off/on-without-hook MIDI
trace must agree exactly for 1,024 frames including sustain, volume and pitch.
This is native compiler evidence, not whole target binary or timing equivalence.
GCC and Clang ASan/UBSan run the new suite. Those sanitizers plus the stress test
are not a TSan/formal proof of freedom from all possible data races.

The freeze guard removes only marked app additions and compares the original GM
and handoff bodies exactly. Existing frozen Host policy, cursor/release math and
source/residency modules cannot drift. Previous Voice/full-bank/target workflows
remain required and are not substitutes for this application's new checks.

## Remaining gates

Normal firmware startup and real immutable-resource identity; device-side bounded
analysis; real-bank original-instrument end-to-end application validation; target
execution of the new transaction wrapper; fault-group recovery integration;
actual RAM/PSRAM capacity, CPU, I2S/DMA and listening. `runtime_approved=false`.
