# Scripts

This directory will contain the shared command surface for local development and Codex Cloud.

Planned commands:
- `setup.sh` — install/verify pinned toolchain dependencies once the baseline is selected
- `build.sh` — reproducible firmware build
- `test.sh` — host/unit tests
- `analyze_sf2.sh` — run the M1 analyzer on a user-supplied SoundFont

The scripts should be idempotent where practical and should avoid embedding machine-specific absolute paths.

Do not pin an ESP-IDF/Arduino version until M0 selects and records the baseline.