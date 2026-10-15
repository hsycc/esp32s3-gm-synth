# N16R8 Program Prepare Benchmark

This is the second physical M3 gate after the internal-Flash -> PSRAM sweep.
It measures the control/storage-domain cost and ownership behavior of real SF2
Program Prepare on an ESP32-S3 N16R8 target.

## What the hardware benchmark exercises

The dedicated `sdkconfig.hwbench.defaults` build runs the existing 4 MiB
Flash -> PSRAM storage sweep first. After `SampleResidencyManager` is initialized
on the same 4 MiB arena, it then runs these real-SF2 cases:

1. `cold_initial` — source-backed initial prepare; PCM is read from the pinned SF2 directly into the residency arena.
2. `warm_same_program` — the same dependency set is prepared again and must reuse already-resident samples without source reads.
3. `warm_commit_retire` — READY -> Active block-boundary commit followed by control-side acknowledgement/retire.
4. `prime_cache` — unrelated real samples are loaded as unpinned/unpublished cache until the arena is under pressure.
5. `eviction_program_change` — a disjoint dependency set is prepared and must use the safe LRU cache-eviction path when capacity is exhausted.
6. `eviction_commit_retire` — the changed program is committed and the old program is retired while a held Voice Ref proves that an old release tail remains valid until explicit release.

All sample data comes from the same pinned `FM_GM_Open_subset_6MiB.sf2` fixture
used by the storage benchmark. `Sf2SampleSource` keeps its production 32 KiB
source-read chunk size.

## Build / flash / monitor

```bash
./scripts/build_hwbench_idf.sh /dev/cu.wchusbserial5AE60242501
idf.py -B build-hwbench -p /dev/cu.wchusbserial5AE60242501 monitor
```

The benchmark is successful only when the log reaches both:

```text
prepare_bench: PASS ...
M3 core linked successfully ...
```

Important case lines are:

```text
prepare_bench: case=cold_initial ok=1 ...
prepare_bench: case=warm_same_program ok=1 ... loaded=0 ...
prepare_bench: case=warm_commit_retire commit_ok=1 ack_ok=1 ...
prepare_bench: case=prime_cache ok=1 ...
prepare_bench: case=eviction_program_change ok=1 ... evictions=... capacity_retries=...
prepare_bench: case=eviction_commit_retire commit_ok=1 ack_ok=1 ... release_tail_survived=1 release_ok=1
prepare_bench: PASS ...
```

QEMU executes the same transaction path as a functional gate, but QEMU elapsed
values are never treated as N16R8 performance evidence.

## Generate a checked report

Save the complete serial output, for example:

```text
reports/local/n16r8-program-prepare.log
```

Then run:

```bash
python -m tools.program_prepare_benchmark.report \
  reports/local/n16r8-program-prepare.log \
  --expected-samples 67 \
  --title "N16R8 Program Prepare / LRU" \
  --output benchmarks/N16R8_PROGRAM_PREPARE.md
```

The parser returns a non-zero exit status if required cases are missing or if it
finds a panic/failure signature. It also enforces the important semantic checks:

- cold prepare must load real PCM bytes;
- warm prepare must perform zero sample loads and reuse the complete A dependency set;
- eviction prepare must perform both source loading and at least one safe cache eviction;
- commit and acknowledgement must succeed;
- the held release-tail Voice Ref must survive retirement and then release cleanly;
- a final `prepare_bench: PASS` line must be present.

## Metrics to retain

For physical qualification keep at least:

- cold prepare elapsed time, dependency bytes, source bytes loaded;
- warm prepare elapsed time and resident-reuse count;
- cache-prime elapsed time and residency occupancy/free-span telemetry;
- eviction Program Change elapsed time, source bytes, eviction count/bytes and capacity retry count;
- block-boundary commit time;
- retire/acknowledgement time;
- release-tail lifetime result;
- final residency high-water and largest free span;
- boot-time internal SRAM heap lines and PSRAM before/after-arena telemetry.

The first physical result is evidence for deciding whether the M3 control/storage
path is ready to freeze before M4 Voice DSP/I2S. Do not derive a performance pass
threshold from QEMU timing.
