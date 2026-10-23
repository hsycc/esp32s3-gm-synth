# Benchmarks

Benchmark reports and reproducible baselines belong here.

Current baseline reports:

- `QEMU_SF2_STORAGE_BASELINE_2026-10-15.md` — first green ESP32-S3 QEMU run of generated Flash FATFS -> VFS -> real SF2 -> PSRAM. Functional integration only; QEMU throughput is not hardware performance data.
- `N16R8_FLASH_STORAGE_BASELINE_2026-10-15.md` — first physical N16R8-class internal-Flash -> PSRAM baseline. Records the 8 MiB Octal-PSRAM reserve, 4 MiB arena result, 4/8/16/32/64 KiB timing sweep and the decision to retain the 32 KiB SF2 read chunk plus 4 MiB arena for M4 entry.
- `GM128_HOST_QUALIFICATION_BASELINE_2026-10-18.md` — completed full-FM host GM128 semantic/residency/lifetime baseline. Records all 5,760 StrictQualified presets, 4 MiB Program Change eviction stress, the 16-channel Active+READY capacity bracket, release-tail reclamation and a deterministic 32-second mixed host soak. It is not hardware render-performance evidence.

Hardware reports should record:
- exact board/module
- commit SHA
- ESP-IDF/toolchain version
- CPU frequency
- PSRAM/flash configuration
- storage medium and filesystem
- SoundFont/bank revision or SHA-256
- benchmark byte count and chunk sizes
- throughput plus min/avg/p50/p95/p99/max latency
- internal SRAM and PSRAM high-water values
- any panic/reboot/cache/storage errors

Audio/DSP benchmark reports should additionally include:
- audio format and block size
- enabled DSP/FX
- active voice count
- render-time statistics
- deadline misses/underruns
- queue drops/sample faults

See `docs/BENCHMARK_PLAN.md` and `docs/HARDWARE_BRINGUP_N16R8.md` for the required measurement methodology.
