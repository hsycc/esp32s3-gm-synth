# QEMU Real-SF2 Storage Baseline — 2026-10-15

## Scope

This report records the first green ESP32-S3 QEMU integration run that exercised the complete generated-flash FATFS -> VFS -> real SF2 parser -> PSRAM benchmark path.

This is a **functional integration baseline only**. QEMU flash timing is emulated and must not be used as ESP32-S3 N16R8 storage-performance evidence.

## Source

- GitHub Actions workflow: `esp-idf-build #21`
- Run ID: `38706743323`
- Commit: `86e492b99ff7fa160971603962ba8ea49a29ba4d`
- ESP-IDF: v6.1
- QEMU machine: ESP32-S3
- virtual flash: 16 MiB
- virtual PSRAM: 8 MiB Octal
- PCM residency arena: 4 MiB

## SF2 fixture

- file: `assets/sf2/subsets/FM_GM_Open_subset_6MiB.sf2`
- SHA-256: `9650eb1c2202e2d92aa3be155e4e895cf6531a241c8ea413fc09e7f9159d4a62`
- file bytes: `5,898,214`
- playable samples: `67`
- `smpl` payload offset: `544`
- `smpl` payload bytes: `5,887,930`
- QEMU benchmark bytes per chunk-size pass: `262,144` (256 KiB)

## Functional result

PASS:

- 8 MiB Octal PSRAM detected
- 4 MiB PCM arena allocated
- generated raw FATFS partition mounted at `/sf2`
- `/sf2/test.sf2` opened through ESP-IDF VFS
- real SF2 parsed successfully
- all 4/8/16/32/64 KiB benchmark passes completed
- residency manager initialized after benchmark
- no panic/assert/Guru Meditation signature

## QEMU telemetry

| Chunk | Reads | Bytes | Elapsed | Throughput | Min us | Avg us | P50 us | P95 us | P99 us | Max us |
|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 4 KiB | 64 | 262,144 | 108,551 us | 2.303 MiB/s | 1,602 | 1,688 | 1,636 | 1,806 | 3,574 | 3,574 |
| 8 KiB | 32 | 262,144 | 105,712 us | 2.365 MiB/s | 3,147 | 3,299 | 3,202 | 3,430 | 5,454 | 5,454 |
| 16 KiB | 16 | 262,144 | 106,180 us | 2.354 MiB/s | 6,285 | 6,632 | 6,408 | 9,863 | 9,863 | 9,863 |
| 32 KiB | 8 | 262,144 | 103,884 us | 2.407 MiB/s | 12,568 | 12,981 | 12,717 | 14,862 | 14,862 | 14,862 |
| 64 KiB | 4 | 262,144 | 103,892 us | 2.406 MiB/s | 25,273 | 25,967 | 25,288 | 27,289 | 27,289 | 27,289 |

Residency after the benchmark:

```text
capacity=4194304
used=0
largest_free_span=4194304
free_spans=1
```

## Interpretation

The important result is not the approximately 2.3–2.4 MiB/s QEMU throughput. The important result is that the complete storage path is now executable under the Xtensa ESP32-S3 target with real SoundFont data and 8 MiB Octal PSRAM semantics.

The next measurement must run on a physical N16R8-class module. The hardware benchmark uses the same fixture and the same code path, but raises the workload to 4 MiB per 4/8/16/32/64 KiB pass. Those results become the first valid Flash -> PSRAM performance baseline.
