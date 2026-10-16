# M6 — Adaptive Polyphony and Safe Voice Stealing

## Goal

Make overload behavior graceful and measurable instead of allowing repeated deadline misses or hard-cut clicks. M6 builds on the measured M4 audio deadline baseline and the bounded M5 event path.

## Work

- measure per-block render duration and deadline misses
- derive rolling CPU/deadline pressure
- separate the compile-time hard Voice cap from the runtime admission limit
- lower admission after true misses or sustained pressure
- recover capacity gradually after sustained stability
- add hysteresis so the admission limit does not oscillate
- add fast-release/fade victim handling for Voice stealing
- define duplicate-note and exclusive-class behavior
- optionally use per-Voice DSP complexity as a predictive admission signal
- record pressure events, steals and current admission limit

## Duplicate-note and exclusive-class policy

- Repeated NoteOn for the same MIDI channel/key is allowed to stack as independent Voices.
- NoteOff releases every currently active Voice matching that MIDI channel/key. This is the explicit V1 duplicate-note policy used by stress tests.
- `exclusive_class == 0` has no exclusive behavior.
- A non-zero exclusive class chokes older sounding Voices only when they are on the same MIDI channel and have the same exclusive class.
- Multiple SF2 zones/layers spawned by one logical NoteOn must share the same non-zero `note_instance_id`; sibling Voices with the same instance ID do not choke each other.
- An un-grouped `note_instance_id == 0` is treated as an independent trigger.
- The incoming sample is acquired and validated before any exclusive victim is changed. A missing/corrupt incoming sample never sacrifices an existing Voice.
- Exclusive victims use the same bounded fast-release fade/tail budget as overload stealing, preventing the two policies from creating unbounded transient CPU work.
- If the shared tail budget cannot cover every matching exclusive victim, the incoming Voice is rejected and no victim is partially choked.
- At the current admission limit, an exclusive replacement may start without overload stealing enabled because it replaces an existing non-tail Voice. If adaptive admission was already lowered below the current non-tail population, exclusive replacement is rejected until overload recovery catches up.
- The hard Voice cap remains absolute: no semantic replacement creates a 33rd active Voice.

The runtime SF2 preset/zone parser is responsible for carrying generator 57 (`exclusiveClass`) and assigning one shared `note_instance_id` to all layers produced by one NoteOn. That parser/data-path work is separate from the Voice-pool policy defined here.

## Validation

- overload lowers new-Voice admission before repeated failure
- stable conditions restore capacity gradually
- counters do not silently overflow
- stealing avoids an abrupt PCM discontinuity
- duplicate same-note stacking and all-match NoteOff are regression-tested
- exclusive-class choke is scoped to MIDI channel, preserves same-NoteOn layers, and uses bounded fast release
- exclusive replacement never mutates an existing Voice before the incoming sample is validated
- exclusive tail-budget and adaptive over-target rejection leave old Voices unchanged
- admission never violates the M3 PublishedVoiceRef lifetime contract
- the hard Voice cap remains a safety ceiling, not a claim of always-safe realtime polyphony

## Exit criteria

The synth has a tested overload policy whose runtime Voice admission follows measured N16R8 headroom rather than assuming `MAX_VOICES` is always realtime-safe.
