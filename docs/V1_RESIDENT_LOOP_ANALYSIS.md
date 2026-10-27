# Bounded resident-PCM loop analysis (default off)

## Scope

Stacked on the verified application transaction at 6187ae12. No frozen Host
policy, cursor, production Voice/resolver, transaction, main startup, SDKCONFIG,
PCM asset, DMA, gain or polyphony changes. Normal builds do not instantiate this
provider. Default provider opt-in is false. All recommendations remain
experimental, not audible approval or a product enable map.

`BoundedLoopAnalyzer` ports the decision-bearing metrics and seven thresholds
of `prepared-loop-review-v3-source-matched-peak`. It considers Direct/16/32/64
source-frame overlap candidates. It computes the original symmetric neighborhood,
source-matched changed-edge peak, local DFT shape and autocorrelation lag. It does
not compute unused report-only diagnostics or copy/render the whole loop. Long
loop length affects period screening, not the number of analyzed endpoint frames.

No thresholds were changed. Direct seam floor 2500; absolute improvement 1800;
relative improvement 0.65; period fraction strictly below 0.01; stored five-decimal
spectral distance strictly below 0.3; zero lag shift; changed-edge peak <= original
Direct peak; neighborhood RMS <= Direct RMS * 1.05. Sorting is residual seam then
shorter fade. A superior but rejected candidate does not hide another valid one.

Integer waveform rounding is exact for the odd overlap denominators 17/33/65.
DFT/lag use bounded double-precision math, not a new perceptual model. Native
cross-validation requires identical choices, rejection masks, rounded spectral
metrics, lag and integer edge attribution. Only unrounded RMS allows absolute
1e-8 diagnostic rounding error; discrete decisions/rejections allow no mismatch.
Neither numerical parity nor the earlier audition establishes global quality.

## Control-only provider and resource safety

`ResidentLoopProposalProvider` is constructed for ONE channel and ONE verified
immutable asset/session. Supply its callback to the existing LoopProgramChannel.
The caller supplies the already derived real Program/Zone context. The provider
checks full identity, revisions, loop mode, actual Zone geometry and membership,
address-modulator absence, descriptor and pinned residency/publication generation.
It acquires an independent temporary directory ref while analyzing real resident
PCM, revalidates publication, then releases the ref before returning value-only
parameters. Handoff independently derives and validates again when admitting.

There is no Sample-ID whitelist, JSON approval map, PCM rewrite, persistent
analysis cache, file read or heap allocation in these two new classes. Source,
Program and PCM must remain immutable under the existing control-owner contract.
Reset/rebind of the owner group and session allocation are still loader duties.
Analysis is never called by NoteOn/render; audio continues to use the old Program
until the existing joint commit protocol has a completed sidecar.

Disabled, inapplicable, stale, invalid, over-budget, cancelled or inconclusive
analyses return no proposal; the transaction retains Direct and proceeds. A partial
analysis cannot publish a candidate from an earlier completed length. Optional
plan capacity remains the existing channel capacity, not the analyzer budget or
polyphony. Prepared order determines which Zones receive the limited budget.

## Explicit bounds, not a timing claim

The core owns 4096 bytes of double scratch and borrows the input pointer only
during analyze(). Endpoint reads are bounded independently of full PCM length.
One complete analysis has a conservative 50000 loop-iteration work allowance;
budget is debited before bounded loops. A checkpoint is reached after at most
4288 charged iterations. It may yield/check a caller deadline and cancel, but
must not throw, reenter or mutate the protected Program/PCM. These are counted
iterations, NOT CPU cycles, microseconds or a physical worst-case execution time.

Defaults: experimental opt-in false, at most 8 analyses and 400000 charged units
per Program transaction. Identity/token changes reset the transaction budget;
repeated calls for the same transaction do not. Max configurable budget is 64
analyses/3200000 units. Counts and work limits independently force Direct. Analysis
reports belong to the serialized control task, not unsynchronized audio telemetry.
No new real-time allocation, locking or analysis is added; existing directory
atomics and production staging costs are not reclassified as wait-free.

The DFT presently uses ordinary double sin/cos and has not been SIMD-optimized.
Preparation latency still needs physical measurement. The scratch is additional
control-side state per configured provider, NOT a copy of the PCM arena. Do not
extrapolate its size to an approved full-device 4.5 MiB fit.

UntilRelease may use a separately configured 0/16/32/64 OUTPUT-frame release
bridge (default 32). Continuous is not converted to UntilRelease. This is distinct
from the automatically selected SOURCE-frame loop fade; existing short-tail
fallback and changed steady-loop-period behavior remain unchanged.

## Verification gates

- Native core tests cover zero/exact/insufficient budgets, cancellation, stale
  result clearing, short-loop boundaries and work independence from loop length.
- Differential tests run deterministic synthetic waveforms and every analyzable
  real-bank effective geometry against the unchanged frozen Python v3 report.
  Full-bank analyzer parity is NOT full-bank application/target/listening approval.
- The resident-provider application uses a new 25866-byte synthetic SF2. Expected
  [32,32,0] choices are generated by frozen Python, not the new C++ implementation.
  Actual parser/source/residency, verified memory reader, MIDI resolver, VoicePool
  and joint Program commits execute; budget/disabled/cancel/identity failures,
  no render-side analysis, old tails and final ref cleanup are asserted.
- The same application fixture is compiled for native GCC/Clang sanitizers and
  explicit Xtensa. The target evidence checker requires the fixed trace/identity,
  normal return, no fatal markers and uniform build macro/ELF audit. QEMU uses
  synthetic internal PCM arena, not PSRAM, I2S or a speaker. Task WDT stays on.

Runtime results must be recorded against their actual commit; submitted CI is
not passing evidence. New code does not imply ordinary firmware main has enabled
automatic analysis. Startup configuration, mutable storage pinning, stopped-group
fault recovery, broad target-bank coverage, physical preparation/render latency,
I2S/DMA, memory budget, listening and enabled fast-path work remain separate gates.
