# GM128 Host Qualification Baseline — 2026-10-18

## Scope

This report records the completed host qualification baseline for the tracked
full FM/GM SoundFont and the production-shaped GM runtime paths.

This is **host semantic, residency and lifetime evidence**. It is not physical
ESP32-S3 render-performance evidence and must not be used to claim guaranteed
polyphony, I2S deadline margin or total N16R8 PSRAM headroom.

## Source

- qualification commit: `917a1f30e3339b69179a7e2197d0e430ded8fdb4`
- GitHub Actions workflow: `gm128-full-bank`, run 8
- bank: `assets/sf2/full/FM_GM_SoundFont_v0_2_1_mini.sf2`
- file bytes: `14,337,918`
- host sample rate used by playback preparation/render tests: `44,100 Hz`
- soak render block: `64 frames`

## Full-bank structure / StrictQualified gate

PASS:

- presets: `5,760`
- banks: `45`
- programs per discovered bank: `128`
- samples: `175`
- `smpl` payload: `14,056,220` bytes
- every discovered preset StrictQualified: `5,760 / 5,760`
- prepare failures: `0`
- metadata failures: `0`
- dependency failures: `0`
- duplicate preset identities: `0`
- missing bank-0 melodic programs: `0`
- mandatory melodic key misses: `0`
- representative key/velocity grid misses: `0`
- standard drum key misses: `0`
- preview zones: `0`
- unsupported modulator records: `0`

The tracked FM asset itself contains no non-zero `pmod` / `imod` records and no
stereo sample pairs. Modulator and stereo compatibility therefore remain covered
by the separate compatibility fixtures rather than this bank.

### Largest single prepared program

Bank `128`, program `0` is the largest prepared program in this asset:

- native zones: `47`
- playback zones: `47`
- sample dependencies: `47`
- logical PCM working set: `2,969,674` bytes
- aligned PCM working set: `2,969,748` bytes
- Prepare scratch/publication storage estimate: `35,940` bytes

The soak discovery additionally proved that those 47 drum zones are a key map,
not a 47-layer NoteOn: the maximum playable NoteOn layer count found across
bank 0 + bank 128, all programs, all MIDI keys and velocities
`1/32/64/96/127`, is `1`.

## 4 MiB single-channel Program Change qualification

### Melodic bank 0

PASS for programs `0 -> 127` through real Program Change Prepare/READY/commit/
ACK transactions:

- transitions after initial activation: `127`
- arena bytes: `4,194,304`
- high-water: `4,184,380` bytes
- safe cache evictions: `92`
- cache bytes evicted: `6,895,564`
- capacity retries: `92`
- prepare/publication invariant failures: `0`

This intentionally reaches very close to the 4 MiB arena boundary and therefore
qualifies the safe-cache eviction path rather than only an easy warm-cache path.

### Percussion bank 128

PASS for programs `0 -> 127`:

- transitions after initial activation: `127`
- high-water: `2,969,748` bytes
- cache evictions: `0`
- capacity retries: `0`

## Runtime-control / GM functional coverage

PASS:

- full-bank Program Change transaction and block-boundary commit/ACK
- channel-10 percussion routing
- 16-channel GM Reset transaction
- real CC0/CC32 Bank Select into a full-bank Program Change
- `128 -> 1` latest-wins rapid Program Change coalescing
- Note On / Note Off
- pitch bend and supported pitch-bend-range RPN path
- CC7 volume
- CC10 pan
- CC11 expression
- CC64 sustain
- CC91 reverb send
- CC93 chorus send
- dense piano
- same-note retrigger
- drum bursts
- mixed 16-channel NoteOn behavior

The short stress workload reached `16` active Voices with `60` successful Voice
starts, `0` Voice-start failures and `0` published-sample acquire misses.

## 16-channel Active + READY capacity bracket

The representative 15-melodic-plus-drum Active set contains:

- unique resident samples: `62`
- aligned PCM bytes: `4,310,396`

Scanning every bank-0 melodic READY candidate while that Active set remains
pinned found the largest union at:

- channel: `0`
- program: `122`
- READY dependencies: `1`
- Active + READY aligned PCM union: `4,504,436` bytes

Real residency/publication/READY/commit/ACK trials produced:

| Arena | Active | READY | Commit | Result |
|---:|:---:|:---:|:---:|---|
| 4.00 MiB (`4,194,304`) | no | no | no | Active baseline cannot fit |
| 4.25 MiB (`4,456,448`) | yes | no | no | worst READY capacity failure |
| 4.50 MiB (`4,718,592`) | yes | yes | yes | PASS |
| 4.75 MiB (`4,980,736`) | yes | yes | yes | PASS |
| 5.00 MiB (`5,242,880`) | yes | yes | yes | PASS |
| 5.50 MiB (`5,767,168`) | yes | yes | yes | PASS |
| 6.00 MiB (`6,291,456`) | yes | yes | yes | PASS |

Decision: `4.50 MiB` is the first tested host arena that admits the scanned
worst representative Active+READY transaction. This is a **host lower bound**,
not yet the firmware production default. N16R8 total-PSRAM measurements must
prove sufficient reserve for the rest of the audio runtime before changing the
current 4 MiB default.

## Discovery-driven release-tail qualification

The bank-wide discovery found:

- maximum playable NoteOn layers: `1`
- largest prepared zone set: bank `128`, program `0`, `47` zones
- playable keys at velocity 96 for that zone set: `47`
- longest discovered volume-envelope release in the scanned bank: `26,465` frames
- selected bounded release test: bank `0`, program `88`, key `0`, velocity `1`
- pre-NoteOff envelope warmup: `151` frames
- repeated NoteOn instances: `8`
- release render until all Voices finished: `26,496` frames
- final published live Voice refs: `0`
- stale release faults: `0`

This specifically validates that overlapping release-tail Voices keep their
sample publication references alive and that those references are reclaimed
cleanly after natural envelope completion.

## Deterministic 32-second host soak

The long host gate renders a fixed amount of audio-domain work, not a wall-clock
performance benchmark:

- blocks: `22,050`
- block size: `64` frames
- rendered audio frames: `1,411,200`
- represented audio duration: `32,000 ms` at 44.1 kHz
- host sample arena: `4,718,592` bytes (4.5 MiB)
- sample-arena high-water: `4,310,396` bytes
- peak active Voices: `16`
- Voice starts: `2,080`
- Voices finished: `2,080`
- rendered Voice frames: `17,141,721`
- sample acquire misses: `0`
- final published live Voice refs: `0`
- stale sample/reference faults: `0`
- deterministic output checksum: `11224005462836535547`

The event pattern repeatedly exercises 16-channel NoteOn/NoteOff, sustain,
live volume/pan/expression/reverb/chorus updates, pitch bend and drum bursts.
The run completed with no VoicePool full/admission failures.

## Host qualification conclusion

The software-side M6 GM128 gate is now substantially complete for:

- full-bank parser/resolver/materializer semantics;
- GM controller/runtime routing;
- Program Change transaction lifetime;
- 4 MiB single-channel cache pressure and eviction;
- representative 16-channel residency sizing;
- worst scanned Active+READY admission;
- release-tail sample lifetime;
- bounded long-running mixed playback.

The remaining M6 release decision is primarily physical N16R8 qualification.
Host/QEMU results must not be promoted into guaranteed realtime voice-count or
deadline claims.

## Remaining physical N16R8 gates

Record on the actual target:

- total/free/largest PSRAM with a candidate 4.5 MiB arena and full audio runtime;
- internal SRAM high-water and largest free block;
- cold/warm/eviction Program Prepare latency;
- 64-frame render min/avg/p95/p99/max timing;
- 16 / 18 / 20 Voice load matrix where reachable;
- dry, reverb, chorus, filter and worst known program paths;
- deadline misses;
- I2S underruns;
- event queue drops;
- invalid sample/fetch accesses;
- Voice steals and steal-tail behavior;
- long-running mixed 16-channel playback stability.

Only that hardware evidence can establish the final guaranteed polyphony and
authorize the production PCM-arena size.
