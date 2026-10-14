# Firmware Layout

The firmware will be split by responsibility rather than accumulated in a single monolithic synth object.

Planned modules:

- `platform/` — ESP32-S3/RTOS/I2S/platform glue
- `audio/` — block renderer, scheduler integration, output buffers
- `midi/` — MIDI transports/decoder/internal event conversion
- `gm/` — 16-channel GM state and semantics
- `sf2/` — SoundFont parsing and zone resolution
- `sample/` — sample handles, PSRAM arena, residency/source abstraction
- `synth/` — voice pool, allocation, stealing and note lifecycle
- `dsp/` — interpolation, envelope, filter, LFO and global FX
- `storage/` — persistent bank/asset access outside realtime path
- `diagnostics/` — timing, underrun, queue, memory and fault telemetry

The actual directory/component build files should be created during M0/M2 once the toolchain baseline is selected.

Realtime constraints are defined in the root `AGENTS.md` and `docs/REALTIME_DESIGN.md`.