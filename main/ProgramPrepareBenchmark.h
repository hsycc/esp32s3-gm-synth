#pragma once

namespace gm_synth::sample {
class SampleResidencyManager;
}

namespace gm_synth::bringup {

// Runs the optional on-target Program Prepare / cache-eviction benchmark.
// Returns true when disabled or when every enabled benchmark case passes.
bool runOptionalProgramPrepareBenchmark(sample::SampleResidencyManager& residency);

}  // namespace gm_synth::bringup
