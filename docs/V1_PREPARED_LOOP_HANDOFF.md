# Prepared loop context and bounded plan handoff

## Scope

This batch adds a reusable experimental adapter and SPSC sidecar. It consumes
production PlaybackProgramSnapshot, ProgramSnapshot, SampleSource,
SampleResidencyManager and PublishedSampleDirectory objects. Host integration
uses real Sf2PlaybackProgramPreparer, Sf2SampleSource, ProgramPrepareController
and both existing snapshot exchanges, not mock ownership or Python-created
C++ Zone metadata. No application, production Voice or Prepare implementation
is modified, and no JSON candidate automatically enables firmware playback.
The Host policy, cursors and pinned ownership layer remain frozen at ab737d2d.

## Independent context construction

PreparedLoopContextBuilder.begin validates matching channel/bank/program/token
and bank generation, plus full playback snapshot validity, outside audio.
derive reads the actual source descriptor, verifies program dependency
membership, resident program pin, full descriptor equality and the currently
published record's PCM pointer and generations. It obtains effective offsets
and loop mode from the production PlaybackZone, not from a candidate report.
All NoteOn address routes are excluded. Mono type 1 is the only enabled type.
Invalid inputs do not write the output context or leave temporary refs alive.

The asset digest/session stamp comes from the trusted loader independently of
the proposal. The loader MUST keep the source immutable and avoid session/token
reuse. This is an identity binding, not cryptographic file authentication. The
Host verifier hashes the actual file before and after testing; an embedded
immutable-asset hashing/session provider remains an application integration task.

## SPSC protocol and memory lifetime

PreparedLoopHandoff<N> manages one channel with two fixed buffers. N defaults to
16 optional plans, not 16 Voices and not a claim that every program fits N.
Full capacity is reported without overwriting existing entries. An empty bundle
is valid and can deactivate plans for the next program. Individual requests
still require explicit experimental_opt_in; no default Crossfade is enabled.

One serialized control task calls begin/add/publish/cancel/acknowledge; one
serialized audio task calls commit at a block boundary and start between those
boundaries. Plans are copied/bound into private staging storage. Control's
Ready release store and audio's acquire load publish complete immutable plans.
Audio swaps the active buffer, then release-stores Committed. Control's acquire
load observes that acknowledgement and only then resets the retired plans.
This prevents control retirement from racing with audio access to an old plan.
Playing voices hold their own independent sample ref and no plan pointer.

Both PRODUCTION sample and playback exchanges must commit first at the same
boundary. The sidecar requires the exact current snapshot pointers and copied
metadata to match. A mismatch rejects that sidecar; it never combines partially
matched program state. The future application must ensure commit ordering and
joint retirement; this helper does not turn two external exchanges into one
atomic transaction. Missing/rejected plans require the caller's existing Direct
fallback. Tokens and source sessions must not be reused while referenced.

Only control resets plan refs after acknowledgement. Audio commit is constant
work; lookup/start is bounded by N, with no source call or new mutex/heap work.
Existing directory acquire/release operations retain their prior implementation,
including its release CAS retry; no strict MCU contention/cycle bound is claimed.
Destruction/resetQuiescent requires both tasks stopped, and the directory must
outlive the sidecar and all playing voices. Never forcibly free live PCM.

## Validation

`bash scripts/test_v1_loop_prepared.sh` compiles real production sources and
runs synthetic-source context/error, capacity/cancel/rejection, real controller
retirement, 36 PCM combinations and 2,000 concurrent prepare/commit/retire cycles.
The synthetic fixture mocks only storage bytes, not residency or directory.
Repeat with CXX=clang++ SANITIZE=1 for ASan/UBSan; neither is a TSan proof.
The independent Python oracle is also cross-checked with the frozen release
oracle, including raw tail and single-frame boundaries.

The real-bank probe prepares a selected Program's FULL dependency list, loads
it through Sf2SampleSource and ProgramPrepareController, derives the actual
selected Zone context, and starts through the sidecar. It then switches both
production exchanges to another prepared program whose dependencies exclude
the tested sample, retires the old sidecar and program
pins, and only then emits the old voice's PCM. The voice must survive that real
retirement, retain exact output, and allow final reclamation/eviction with zero
live-ref or stale-release faults. Every frozen Host candidate and the explicit
604/602/62 reference geometries are compared at multiple rates/block sizes.

Host arena capacity covers both full dependency sets during READY and is reported;
this is NOT an R8-fit or memory-budget qualification. No PCM or SF2 is uploaded:
only JSON metrics/hashes and native test logs. No audible PASS or hardware timing
is inferred from exact PCM equality, compiler success or sanitizer results.

## Remaining production gate

The adapter and handoff are not called by the current firmware app. Integration
still needs immutable source identity provisioning, safe joint program/sidecar
commit ordering, default-off production Voice reference and fast-path hooks,
envelope/sustain/full-FX tests, and measured N16R8 CPU/I2S/DMA. No merge or release
is authorized by this experiment; the overlap's period and listening caveats
remain unchanged. Do not retune the frozen scorer to accommodate this adapter.

Production ProgramPrepareController rejects empty dependency lists. The integration
test must switch to a valid replacement program, never change that production
contract merely to simplify the test. CI uses explicit pipefail so a compiler or
assertion failure cannot be masked by tee while saving diagnostic logs.
