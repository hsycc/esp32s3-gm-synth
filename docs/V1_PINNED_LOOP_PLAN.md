# Experimental loop plan identity and PCM lifetime

## Frozen scope

This batch wraps the `7f4c192f` cursor/release code in an ownership layer.
It does not modify the publication directory, production Voice, preparation,
Host policy, PCM assets, SDKCONFIG, DMA, gain or admission limits. Both plans
and execution remain opt-in experiments; neither PR is authorized to merge.
No existing firmware or app calls the new classes.

## Two independent pins

`PinnedLoopPlan::bind` acquires one existing `PublishedVoiceRef` while checking
and freezing the request. `PinnedLoopPlayback::start` separately acquires the
CURRENT directory entry and requires the identical resident-record pointer,
bank/sample generations, sample dimensions/rate and an identical playback
context. While the plan pin is held, its record cannot be reclaimed/reused; a
pointer comparison is therefore not an unguarded address-only ABA check.

The caller must provide CURRENT loader/resolver context independently of the
proposal: asset SHA-256, nonzero session serial, bank/sample generations, sample
ID, resident-relative start/end/loop range, loop mode, type, fixed NoteOn-address
status, policy revision and coordinate convention. Every field is compared.
The resident record independently verifies generations, ID, frames, bytes and
rate. The existing directory does NOT contain the asset digest, source offset,
SF2 type or Zone semantics: those fields remain trusted-loader inputs, NOT an
on-device cryptographic check. No JSON-to-firmware plan loader is implemented.
Do not call this with raw absolute `shdr` coordinates or claim asset provenance
from a caller-filled hash alone. Runtime source verification/mapping remains a
separate integration gate.

A bound plan is immutable: private members, value-returning accessors, no copy
or move, and failed rebinding leaves it intact. Playback stores its own cursor
parameters, not a plan pointer. Retirement of the plan does not invalidate
already sounding PCM. One playback pin covers both primary and release shadow.
All readers are reset BEFORE that pin is returned on natural finish, reset or
destruction. Failed acquisitions and validation release temporary pins.
The two owner types cannot accidentally copy or move a refcount token.

```
bind plan -> plan pin
start playback -> independent playback pin
unpublish old sample -> no new old-sample acquisitions
replace same ID -> new starts through old plan rejected
retire plan -> old playback still valid
last playback ends/resets -> canReclaim(old record) -> control frees PCM
```

Plan and playback are not persisted across reset/session change. New starts
never use a retiring record just because the plan still pins it. Re-publishing
the SAME immutable record is not considered a different sample; changing its
fields while pinned violates the existing directory contract. The caller must
keep the directory alive and obey `unpublish -> canReclaim -> free/reuse` for
both record and PCM. Plan pins use the existing voice_refs counter; stats now
include these pins in the experimental harness, not only sounding voices.

## Synchronization and realtime boundary

This is NOT a new lock-free plan table or READY/Commit/Retire scheduler.
Plan bind/reset must not race with playback.start; a caller-controlled safe
handoff/block boundary is required. Once start succeeds, the plan may retire
without touching the playback. Calls on one playback are single-owner and
serialized. The concurrent test deliberately keeps the PLAN immutable while
the control writer changes directory publication, and uses distinct playback
objects in audio threads. It does not test racing plan destruction.

No new allocation, hashing, I/O or mutex is added to either class. Hot sample
reads retain the existing cursor arithmetic; ownership calls reuse directory
acquire/release. In particular directory release contains a CAS retry loop:
this batch does NOT certify a strict worst-case cycle bound under contention,
or change that existing implementation. Plan setup/pinning is control-side;
production audio-side ownership timing remains unmeasured.

## Verification

`bash scripts/test_v1_loop_lifetime.sh` links the real, unchanged directory.
`CXX=clang++ SANITIZE=1 bash scripts/test_v1_loop_lifetime.sh` repeats with
ASan/UBSan. Tests cover context-field mismatch, missing opt-in, invalid lengths,
record generation/descriptor mismatch, cross-directory misuse, same-ID
replacement, old PCM tail survival, multiple geometries, scoped destruction,
last-reference reclamation, and inactive calls after actual PCM deletion.
A fixed matrix compares 324 mode/rate/fade/NoteOff/block combinations against
the frozen release cursor. Two reader threads attempt 4,000 starts while one
writer removes/replaces publication; no plan object is concurrently mutated.
This stress is not a general proof of thread safety or a TSan run.

The pinned Host probe uses the unchanged execution verifier for all 50 Host
candidates and explicit 604/602/62 geometries. It retires the plan AND unpublishes
the sample before emitting any compared PCM; reclamation must remain blocked
until playback ends. This probe has a synthetic session identity. The Python
verifier authenticates the actual bank separately; no claim that a production
bank loader is integrated follows from that harness.

## Remaining gates

Real asset-to-resident identity construction, synchronized plan publication,
production reference/fast-render paths and envelope/sustain/full-FX routing,
listening and N16R8 CPU/DMA/I2S qualification are not implemented/approved here.
No audible or performance PASS, no automatic runtime selection, no stereo policy.
