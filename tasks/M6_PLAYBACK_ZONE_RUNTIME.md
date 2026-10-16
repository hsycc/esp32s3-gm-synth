# M6 Playback Zone Runtime

## Goal

Connect raw realtime MIDI NoteOn/NoteOff events to prepared SF2 playback metadata and the existing VoicePool without parsing SF2 or allocating memory in the audio domain.

## Phase F scope

- Keep sample residency/dependency ownership in `sample::ProgramSnapshot`.
- Add a separate immutable `gm::PlaybackProgramSnapshot` referencing caller-owned prepared playback zones.
- Bound realtime scanning to 256 zones per prepared program and each NoteOn result to the existing 32-Voice hard cap.
- Keep zone backing arrays outside the snapshot object; their lifetime extends from READY publication through retire acknowledgement.
- Validate every checked-in `*.analysis.json` program against both the 256-zone realtime scan bound and `VoicePool::kHardVoiceCap` for maximum simultaneous key/velocity overlap.
- Match key and velocity ranges in the audio domain using a fixed scan only.
- Carry SF2 `exclusiveClass` into `VoiceStartParams`.
- Assign one shared non-zero `note_instance_id` to every layer produced by one logical NoteOn.
- Support fixed-pitch zones and integer equal-temperament key tracking without `pow()`, allocation or storage I/O.
- Feed resolved zones through the existing VoicePool duplicate-note/exclusive-class policy.
- Provide a compatibility coordinator so residency READY and playback READY must identify the same channel/bank/program/token/bank-generation before block-boundary commit.
- Regression-test the complete `RealtimeEvent -> ZoneResolver -> VoicePool -> render` path on Host CI.

The current checked-in GeneralUser subset already contains 200-zone piano programs, so the previous 128-zone assumption was invalid. The 256-zone bound is therefore a tested product constraint rather than an arbitrary inline-array size. The manifest gate also prevents a future bank from requiring more simultaneous layers than the 32-Voice runtime can represent for one NoteOn.

## Deliberately not in Phase F

- Runtime parsing of SF2 `phdr/pbag/pgen/inst/ibag/igen`.
- Full SF2 `scaleTuning` values other than the prepared fixed-pitch/equal-temperament representation.
- Modulator evaluation in the audio callback.
- Storage I/O or heap allocation in NoteOn/NoteOff handling.
- Physical-board polyphony guarantees; QEMU remains the regression environment and N16R8 remains required for final realtime performance evidence.

## Next phase

Build the control-domain SF2 preset/instrument/generator parser that materializes `PlaybackProgramSnapshot` plus caller-owned zone backing storage from a real bank and publishes it alongside the already-prepared residency snapshot. The parser must merge global/local preset and instrument generators, preserve key/velocity ranges, sample identity, loop/tuning/gain/pan/envelope/filter state, and generator 57 (`exclusiveClass`).
