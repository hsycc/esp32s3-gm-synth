# M6 physical qualification report

After flashing the dedicated N16R8 qualification image, save one complete first-boot serial log. The same log contains PSRAM/internal-SRAM snapshots, the 4 MiB Flash→PSRAM sweep, Program Prepare telemetry and the 1/4/8/12/16/18/20/24-Voice render matrix.

Generate the checked report with:

```bash
python -m tools.m6_qualification.report reports/local/n16r8-m6.log \
  --output benchmarks/N16R8_M6_QUALIFICATION.md
```

Default validation expects:

- 8 MiB PSRAM;
- 4.5 MiB (`4,718,592` bytes) qualification PCM arena;
- 4 MiB storage sweep and 67-sample pinned SF2 fixture;
- `timing_scope=hardware physical_timing_evidence=1`;
- all eight fixed Voice cases present;
- zero clock faults, duration clamps, leaked Voice refs and stale residency refs;
- the 1–16 Voice cases within the 1451 µs 64-frame deadline.

18/20/24 Voices are stretch cases. A deadline miss there is recorded as measured capacity pressure rather than making the report invalid. The highest within-deadline Voice count is reported as an observed benchmark result only, not final guaranteed polyphony.

This report intentionally does not claim I2S underrun qualification. Final guaranteed polyphony still requires the real I2S/DMA path plus long-running mixed-MIDI stress.
