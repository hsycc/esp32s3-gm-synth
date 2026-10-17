# N16R8 M6 Qualification Report

This tool consumes the **single complete serial log** produced by the dedicated
physical N16R8 M6 qualification image and renders one checked Markdown report.

It combines the existing storage and Program Prepare parsers with the physical
render-timing matrix and PSRAM/internal-heap snapshots. It rejects QEMU timing as
physical evidence and treats 16 Voices as the current core target. Higher
18/20/24-Voice rows are reported as stretch data.

```bash
python -m tools.n16r8_qualification.report \
  reports/local/n16r8-m6.log \
  --output benchmarks/N16R8_M6_CORE_QUALIFICATION.md
```

A `CORE PASS` requires:

- the pinned 67-sample SF2 storage sweep to pass for all five chunk sizes;
- a 4.5 MiB (`4718592` byte) residency arena;
- Program Prepare cold/warm/eviction/commit/retire gates to pass;
- physical timing markers (`timing_scope=hardware`, `physical_timing_evidence=1`);
- all `1/4/8/12/16/18/20/24` render rows to be present;
- the 16-Voice row to have zero deadline misses, zero clipping and max render
  time no greater than the 1451 us 64-frame budget;
- zero stale residency references and valid arena/high-water accounting;
- complete PSRAM and internal-heap snapshots before/after each phase.

`M6_QUAL_MEMORY allocation_failures` is **pressure telemetry**, not an automatic
failure condition. `SampleResidencyManager` increments it on an attempted load
that hits `NoContiguousSpace`/`MetadataFull`; this benchmark intentionally fills
the evictable cache until that happens, then verifies that Program Prepare can
safely evict and retry. The report therefore preserves this counter and checks
that it is consistent with successful `capacity_retries`, while unrecovered
Prepare failures still fail the qualification.

18/20/24 Voices remain stretch measurements. Missing rows are invalid evidence,
but a deadline miss or clipping above the required 16-Voice target only lowers
the observed contiguous within-deadline candidate; it does not invalidate an
otherwise clean 16-Voice core result.

`CORE PASS` deliberately does **not** claim final release qualification or
"guaranteed polyphony". Physical I2S underruns, end-to-end real-bank mixed
playback and long-running musical stress remain separate hardware gates.
