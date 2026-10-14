# SF2 Analyzer

Host-side M1 tool for turning SoundFont compatibility and memory questions into data before firmware architecture depends on assumptions.

The implementation uses only the Python standard library. Do not commit proprietary or redistributability-unclear `.sf2` banks to this repository.

## Run

From the repository root:

```bash
python -m tools.sf2_analyzer path/to/GM.sf2 --pretty
```

Write a CI-friendly JSON report:

```bash
python -m tools.sf2_analyzer path/to/GM.sf2 \
  --json build/gm-sf2-report.json
```

Evaluate full-residency against a concrete PSRAM budget:

```bash
python -m tools.sf2_analyzer path/to/GM.sf2 \
  --psram-mib 16 \
  --reserve-mib 1.5 \
  --json build/gm-sf2-report.json \
  --pretty
```

`--reserve-mib` is deliberately explicit rather than silently assuming how much PSRAM the firmware may consume outside sample PCM.

## Report contents

The JSON report currently includes:

- RIFF / LIST / SF2 chunk structure
- file size and `smpl` payload size
- preset / instrument / sample counts
- preset and instrument generator opcode usage
- non-zero `pmod` / `imod` records
- linked / left / right sample detection
- per-program resolved zone count
- per-program unique sample set
- raw and 4-byte-aligned per-program PCM working-set size
- representative maximum overlapping zones for a key/velocity point
- conventional GM bank-0 and bank-128 drum summaries
- all-preset and bank-0 sample unions
- greedy 16-program union stress estimate
- greedy 15 melodic + 1 drum union stress estimate
- optional full-residency decision for a supplied PSRAM/reserve budget

The greedy 16-channel working-set estimates are intentionally labelled as heuristics; they are not claimed to be an exact combinatorial worst case.

## Tests

```bash
python -m unittest discover -s tests -v
```

Tests build tiny valid SF2 byte streams in memory, so the test suite does not require a third-party SoundFont file.

## Current scope

This analyzer resolves the dependency path needed for M1:

`preset zone -> instrument -> instrument zone -> sample`

It intersects preset/instrument key and velocity ranges to estimate active layers. It reports modulator presence but does not yet interpret full SoundFont modulator semantics or render audio.

The fixed production GM bank remains the compatibility authority: when its report is available, use the observed feature set to drive firmware support decisions.
