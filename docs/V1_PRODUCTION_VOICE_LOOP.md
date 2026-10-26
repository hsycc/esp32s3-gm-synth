# Default-off production Voice loop integration

## Scope

Stacked on frozen PR #21 at `6d954a7bc200cd0ec300873e7e106fff2a87a692`.
The Host selection policy, PCM, source/prepare/residency, original cursor and
bounded release math are unchanged. No application, Kconfig, DMA, gain,
polyphony or arena configuration enables this path. No merge is authorized.

`GM_SYNTH_EXPERIMENTAL_VOICE_LOOP` defaults to 0. All translation units must use
the same setting. At 0, the Voice/VoiceStartParams layout and compiled native
VoiceRenderer, VoiceEffectsBus and VoiceRuntimeControl objects are compared
byte-for-byte with the frozen baseline. This is native compiler evidence, not
an on-target binary or timing comparison.

At 1, each Voice start still defaults to Direct. Explicit opt-in requires an
immutable PinnedLoopPlan and independently prepared current context, borrowed
only for the synchronous start. A rejected plan causes start failure; a caller
may explicitly retry Direct. No report/JSON automatically enables firmware.

## Binding and ownership

Voice first acquires its existing PublishedVoiceRef. The plan checks exact
resident pointer, directory, generations and complete context; Voice separately
checks the effective start/end/loop, mode and mono descriptor against its actual
VoiceStartParams. A matching generation with a different resident pointer is
rejected. No extra playback ref or PCM copy is allocated. Voice never retains
the plan/current pointers; it copies the bounded cursor state and clears them.
Primary and release-shadow readers reset BEFORE the original ref is released.

PreparedLoopHandoff gains an additive `startVoice` method under the compile
switch. Its existing SPSC readiness/retirement protocol is unchanged. Current
snapshot pointers, channel, Zone index and key/velocity membership are checked;
Voice then validates the resolved playback geometry. The caller must serialize
starts/commit and enforce the existing joint snapshot commit/ack order. The app
is not yet wired to this method. Asset identity remains a trusted loader input,
not authentication by comparing caller-provided hashes.

## Rendering and events

The source hook runs after the existing envelope/modulation updates, before the
existing low-pass, stereo gain, pan and effects-send stages. Current modulated
Q32.32 step is forwarded once per output frame. The cursor advances exactly
once; Voice synchronizes phase rather than wrapping it again with legacy bounds.

An opt-in Voice entering renderAddRealtime currently delegates to renderAdd.
This preserves DSP/event ordering; it is NOT an optimized Crossfade fast path.
Direct voices retain existing optimized paths. In particular, no new guarantee
of 24-voice performance or the I2S deadline follows from Host PCM exactness.

NoteOff, sustain-pedal deferred release, fast release/exclusive choke, and sound
off keep production envelope/VoicePool semantics. UntilRelease passes NoteOff
to the bounded bridge; Continuous keeps looping while its amplitude envelope
releases. Ending the amplitude envelope can stop the source before a bridge
finishes; no lifetime is extended. The previous one-frame-tail/rate-change
fallback and shortened steady-loop period limitations still apply.

## Evidence produced by the new workflow

- Frozen legacy source bodies are checked exactly. Only additive methods are
  allowed in PinnedLoopPlan/PreparedLoopHandoff; existing cursor/policy/source
  modules remain unchanged. Native default-off object and layout parity is
  checked independently, not by simply permitting all Voice changes.
- The same 72-case no-plan trace is compiled against the old source, new default
  build, and new enabled build. PCM and state hashes must be identical.
- 216 combinations compare opted-in Voice reference and realtime entry points
  with an ordinary Voice consuming a pre-rendered stream from the frozen
  cursor. Profiles include envelopes, static/dynamic low-pass, modulation gain,
  live gain and CC21 updates. This uses the frozen cursor as a transport oracle;
  it does not independently prove the cursor mathematics anew.
- Three 2,500-frame pitch-modulation cases forward Voice's current modulation
  step to a separate frozen cursor and compare source PCM/phase. This tests the
  integration of rate changes, not an independent proof of the LFO algorithm.
- Four VoicePool cases compare dry output, reverb/chorus sends and global wet
  output with the streamed oracle, including sustain-deferred NoteOff and live
  gain/send/filter updates. Forced stop/exclusive/steal-release entry points
  also verify ref cleanup. This is not an all-GM-program Full-FX listening test.
- Existing fast/release/send regression binaries run with the compile switch on
  and no plan. GCC and Clang ASan/UBSan execute all native integration suites.
- The real-bank probe uses actual Prepare/Source/Residency and handoff, starts
  production Voice, then switches to a disjoint Program and retires the plan
  and program pins BEFORE rendering old audio. All selected Host candidates
  and 604/602/62 reference geometries are compared with the independent Python
  transport oracle plus Q15 release math. For this probe gain/envelope/DSP are
  normalized (unity gain, 256-frame amplitude release, no filter/modulators).
  Actual Zone loop modes/regions are retained. Full original instrument DSP
  settings, stereo, listening, memory budgets and hardware timing are NOT_RUN.

## Remaining gates

Actual application identity provisioning and atomic-order commit orchestration;
optional-plan capacity/admission policy; production resolver/VoicePool hook;
optimized active Crossfade path if needed; on-target memory/CPU/DMA/I2S A/B;
listening and release edge cases. Keep runtime approval false and PR Draft until
those gates are separately decided. A successful IDF default build does not
show that the compiled-out experimental path executed on an ESP32-S3.
