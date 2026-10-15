# Scripts

Shared command surface for local development and CI.

Current commands:

- `build_idf.sh` — production/default ESP-IDF build for the ESP32-S3 N16R8 baseline.
- `build_hwbench_idf.sh [PORT]` — build the dedicated physical N16R8 Flash -> PSRAM storage benchmark; when `PORT` is supplied, also flash it.
- `run_qemu_ci.sh` — basic ESP32-S3 QEMU boot/PSRAM smoke test.
- `run_qemu_sf2_ci.sh` — QEMU integration test using a generated raw-FATFS partition containing a pinned real SF2 fixture.
- `test.sh` — host/unit tests.
- `analyze_sf2.sh <file.sf2>` — run the SoundFont analyzer.

All ESP-IDF scripts expect `IDF_PATH` to point at the pinned ESP-IDF v6.1 checkout (normally set by sourcing `export.sh`).

The hardware benchmark and QEMU SF2 integration intentionally use the same pinned fixture so parser/storage behavior can be compared without changing SoundFont content. QEMU storage throughput is never treated as physical hardware performance evidence.
