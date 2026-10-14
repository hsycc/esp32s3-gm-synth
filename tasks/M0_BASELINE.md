# M0 — Baseline

## Goal
Establish a reproducible ESP32-S3 audio/SF2 baseline before structural refactoring.

## Work
- Select the exact ESP32-S3 board/module and toolchain baseline.
- Reproduce the relevant SF2 reference behavior at 44.1 kHz stereo.
- Bring up I2S output.
- Add block render timing and memory telemetry.
- Build a repeatable dense-MIDI stress input.
- Measure 8, 12, 16, 19 active voices where hardware permits.

## Deliverables
- reproducible build instructions
- hardware configuration record
- baseline timing/memory report
- known limitations list

## Exit criteria
No major architecture decision should be justified by assumed performance; baseline hardware data exists.