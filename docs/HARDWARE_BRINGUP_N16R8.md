# ESP32-S3 N16R8 Hardware Bring-up

This checklist is the first real-hardware gate for M0 / M3 Phase C.

Reference target:
- ESP32-S3-WROOM-1-N16R8 class module
- 16 MiB flash
- 8 MiB octal PSRAM
- ESP-IDF v6.1
- 240 MHz CPU
- initial PCM residency arena: 4 MiB

The first board run is intentionally limited to platform and residency validation. I2S, MIDI, Voice DSP and effects are not part of this gate.

## 1. Build

Use the repository-pinned ESP-IDF release:

```bash
. "$HOME/esp/esp-idf/export.sh"
./scripts/build_idf.sh
```

Expected build target:

```text
IDF_TARGET = esp32s3
Flash      = 16 MB
PSRAM      = enabled, octal, 80 MHz
PCM arena  = 4194304 bytes
```

The GitHub `esp-idf-build` workflow must also pass before using a commit as a hardware baseline.

## 2. Flash and monitor

Typical local flow:

```bash
idf.py -p <PORT> flash monitor
```

Keep the complete first-boot serial log. Do not edit the reported memory numbers before attaching them to a benchmark/report.

## 3. Required boot evidence

A passing first boot must show all of the following:

1. firmware identifies itself as the M0/M3 bring-up target;
2. ESP-IDF version is printed;
3. SPIRAM-capable heap reports roughly the expected R8-class capacity;
4. the largest PSRAM free block is at least the configured 4 MiB arena request;
5. the PCM arena allocation succeeds;
6. arena address satisfies `SampleResidencyManager::kAlignment`;
7. `SampleResidencyManager::begin()` succeeds over the PSRAM arena;
8. post-allocation PSRAM telemetry is printed;
9. residency telemetry reports zero used bytes and one large initial free span;
10. there is no reboot loop, cache error or PSRAM initialization warning.

## 4. Record these values

Capture the exact serial-log values for:

```text
ESP-IDF version
PSRAM total before arena
PSRAM free before arena
PSRAM largest free block before arena
PSRAM minimum free before arena
Requested PCM arena bytes
PSRAM free after arena
PSRAM largest free block after arena
Residency capacity bytes
Residency largest free span bytes
```

These measurements decide whether the production arena remains 4 MiB or can safely move toward 5–6 MiB. Do not infer this from the nominal 8 MiB module size alone.

## 5. Pass / fail rule

### Pass

- build is clean under ESP-IDF v6.1;
- N16R8 configuration assertions pass;
- 4 MiB SPIRAM arena allocation succeeds on target;
- residency manager initializes successfully;
- measured remaining PSRAM is recorded for later Voice/FX budgeting.

### Fail

Treat any of the following as a real platform issue rather than bypassing the check:

- detected PSRAM substantially below the R8 expectation;
- largest free PSRAM block below 4 MiB at clean boot;
- arena allocation failure;
- unexpected Quad/Octal PSRAM mismatch;
- flash-size mismatch;
- residency initialization failure;
- repeated reset or cache/PSRAM errors.

## 6. What comes after this gate

Only after the above evidence is captured:

1. add an ESP-IDF `RandomAccessReader` backend for the chosen SF2 storage location;
2. load a real SF2 sample through `Sf2SampleSource -> SampleResidencyManager` on hardware;
3. measure storage-to-PSRAM load throughput and latency;
4. re-run Program Prepare / cache-eviction scenarios on target;
5. then begin M4 Voice DSP and I2S output integration.

This ordering keeps storage/PSRAM faults separate from audio-DSP timing faults.
