# M8 `bb31908b` Three-run N16R8 Physical Validation

**Result: 3/3 physical hard gates passed; provenance remains conditional due to `-dirty`.**

Source: three user-supplied full serial logs, with embedded compile date October 24, 2026. No wall-clock run duration or multi-hour soak evidence is inferred. 44.1 kHz, 64-frame blocks, 8 × 64 DMA, master Q15 32767, ES8311 -12 dB.

| Metric | Run 1 | Run 2 | Run 3 |
|---|---:|---:|---:|
| App version | bb31908-dirty | bb31908-dirty | bb31908-dirty |
| ELF SHA prefix | 8a66d5396... | 8a66d5396... | 8a66d5396... |
| Music checksum | fe69915701738934 | fe69915701738934 | fe69915701738934 |
| Render avg us | 736 | 736 | 736 |
| Render max us | 2196 | 2195 | 2195 |
| Soft overruns | 4 | 4 | 4 |
| Max backlog us | 749 | 749 | 749 |
| DMA min margin us | 10861 | 10861 | 10861 |
| Normal stable voices | 24 | 24 | 24 |
| Heavy stable voices | 8 | 8 | 8 |
| Events applied | 128 | 128 | 128 |
| CC21 updates | 3 | 3 | 3 |
| I2S PASS count | 2 | 2 | 2 |
| WDT/Panic lines | 0 | 0 | 0 |
| Hard gate PASS | True | True | True |
| Completed app_main | True | True | True |

## Like-for-like worst-block comparisons

| Scenario | M7 range (us) | M8 range (us) | Interpretation |
|---|---:|---:|---|
| 4 filtered NoteOn + 4 CC events | 2286–2292 (M7 rank 1) | 2100 (M8 rank 2) | approximately 8.1–8.4% lower |
| 24 simultaneous NoteOn | 2125 (M7 rank 2) | 2195–2196 (M8 rank 1) | approximately 3.3% higher |
| overall slowest | 2286–2292 | 2195–2196 | about 4% lower, but dominant scenario changed |

M8 worst 24-NoteOn stage breakdown: Event Apply 741–742 us, Voice Render 1209–1210 us, NoteOn Apply 712–715 us. M8 filtered scene Event Apply 796 us, Voice Render 1020 us, NoteOn Apply 626–627 us, other CC Apply 54 us, CC120 Apply 102–103 us. M7 subtype timing did not exist in the original baseline and cannot be compared as a like-for-like instrumentation measurement.

Average render 736 us in M8 vs 718 us in prior M7 summary (+2.5%), max backlog 749 vs 840–846 us (lower), DMA minimum margin 10861 vs 10764–10770 us (higher). Instrumentation/workload interpretation should remain cautious.

## Evidence boundaries

- All three boots identify as `bb31908-dirty`, ELF SHA prefix `8a66d5396`; this establishes matching printed identity prefixes, not proof of complete binary identity or clean source-binary equivalence.
- Monitor was invoked from the original working directory (not the suggested detached worktree). Review `git status`, `git diff`, staged diff, full ELF SHA and build config used at build time.
- Software clipping counters 0 and hard gate pass do not replace listening for actual pops or checking analog speaker clipping.
- Expected unstable 12-heavy-voice boundary is characterization, not a hard gate failure.
- PR #17 earlier `95aa189-dirty` provenance must also be resolved before unconditional release merge.

## Disposition

Treat M8 hardware realtime/semantic gates as passed in these three logs, but **hold unconditional code release/PR merge** until the dirty-build provenance is reconciled. Proceed to controlled listening and final compatibility/release claims.

## Evidence retention / hashes

Raw files remain the user-provided attachments. Hashes below are SHA-256 of the
original bytes, including ANSI sequences and line endings; no developer-machine
path or full raw log has been committed here.

| Input | Bytes | SHA-256 |
|---|---:|---|
| m8-run1.log | 41679 | `6f2431cfb3e5db3d483fa0b810f4710f37aed64501eab19c13bc97273a5f432d` |
| m8-run2.log | 41681 | `138491931dc686f88b5a10ebee427011b4e912f6a57089773734c5401b24a114` |
| m8-run3.log | 41620 | `ca39ca7212807aa19330a498e4cc8b276a6d5d22a1b6798cc39ac5ed6b5b73cf` |

## Additional boundaries

Each log has one boot-version record and one `Returned from app_main()` at
device uptime 395.404 seconds. Each of its two measured phases has 131072
64-frame blocks. This is three independent short qualification runs, not an
uninterrupted multi-hour soak or a field-reliability claim.

Clean-SF2 checksums are all `978339c9263cfd8c`; stress checksums are all
`fe69915701738934`. Clean render avg/max is 124/1012 us. Stress render avg/max
is 736/(2196,2195,2195) us. Final backlog is zero. Main-task stack watermark is
1916 bytes; measured stress audio-task watermarks are 5008/5040/5040 bytes.

The M7 comparison values above are from the previously retained M7 summary,
not a new simultaneous A/B build. Both M7/M8 dirty provenance and changed
diagnostic instrumentation limit causal performance attribution. The filtered
scene Event Apply is 796 us vs the earlier reported 989 us, but the 24-note burst
and mean render time are higher, so there is no universal speedup claim.

Startup-prime queue overflow is logged as diagnostic-only (1), while active
synth queue overflow is 0. Heavy 12-voice failure is a deliberate boundary,
not a long-run failure. `allocation_failures=13` is present in memory telemetry;
it must not be rewritten as zero or used as proof of a leak without allocation
context. `stale_reference_faults=0` and both hard gates pass.

Next steps and merge restrictions: `docs/V1_RELEASE_CLOSEOUT.md`. Audible
worksheet: `docs/V1_AUDIBLE_ACCEPTANCE.md`. No firmware change is justified
solely by archiving this evidence.
