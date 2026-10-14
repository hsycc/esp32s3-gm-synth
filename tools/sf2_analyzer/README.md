# SF2 Analyzer

Host-side M1 tool for turning SoundFont compatibility and memory questions into data before firmware architecture depends on assumptions.

The implementation uses only the Python standard library. Do not commit proprietary or redistributability-unclear `.sf2` banks to this repository.

## Hardware context

The project target is **ESP32-S3 N16R8**:
- 16 MB flash
- 8 MB PSRAM

Analyzer PSRAM calculations should therefore use 8 MiB as the physical ceiling. `--reserve-mib` is still explicit because the safe non-PCM reserve must be measured on real firmware.

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

Evaluate full-residency against N16R8 with an example reserve:

```bash
python -m tools.sf2_analyzer path/to/GM.sf2 \
  --psram-mib 8 \
  --reserve-mib 1.5 \
  --json build/gm-sf2-report.json \
  --pretty
```

The 1.5 MiB reserve above is an example, not a validated product number. Hardware measurements must establish the real reserve requirement.

## Report contents

The JSON report currently includes:

- source SHA-256 and file size
- RIFF / LIST / SF2 chunk structure
- `smpl` payload size
- preset / instrument / sample counts
- bank -> preset-count distribution
- preset and instrument generator opcode usage
- non-terminal `pmod` / `imod` records
- separate counts for modulator records whose `amount` is non-zero
- linked / left / right sample detection
- source sample-rate distribution
- per-program resolved zone count
- per-program unique sample set
- raw and 4-byte-aligned per-program PCM working-set size
- representative maximum overlapping zones for a key/velocity point
- conventional GM bank-0 and bank-128 drum summaries
- all-preset and bank-0 sample unions
- bank-0 + standard-drum union
- greedy 16-program union stress estimate
- greedy 15 melodic + 1 drum union stress estimate
- optional full-residency decision for a supplied PSRAM/reserve budget

The greedy 16-channel working-set estimates are intentionally labelled as heuristics; they are not claimed to be an exact combinatorial worst case for arbitrary future banks.

## Current real candidate banks

Two candidate files have been analyzed locally:
- `TimGM6mb.sf2`
- `GMBK5X128.sf2`

Their measured comparison is in `docs/SF2_BANK_ANALYSIS.md`. The source binaries are not committed.

## Tests

```bash
python -m unittest discover -s tests -v
```

Tests build tiny valid SF2 byte streams in memory, so the test suite does not require a third-party SoundFont file.

## Current scope

This analyzer resolves the dependency path needed for M1:

`preset zone -> instrument -> instrument zone -> sample`

It intersects preset/instrument key and velocity ranges to estimate active layers. It reports modulator records and amount usage but does not yet interpret full SoundFont modulator semantics or render audio.

The selected production bank/profile remains the compatibility authority: use observed feature sets to drive firmware/runtime and offline compiler decisions.
