#pragma once

namespace gm_synth::bringup {

// Optional deterministic Voice DSP / scheduler render benchmark used by QEMU
// CI. Returns false if any workload or lifetime invariant fails.
bool runOptionalRenderTimingBenchmark();

}  // namespace gm_synth::bringup
