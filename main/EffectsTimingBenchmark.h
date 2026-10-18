#pragma once

namespace gm_synth::bringup {

// Regression-only effects timing probe. QEMU values are used only for relative
// comparisons; physical timing remains the product qualification source.
bool runOptionalEffectsTimingBenchmark();

}  // namespace gm_synth::bringup
