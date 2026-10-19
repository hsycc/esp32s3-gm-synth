#pragma once

namespace gm_synth::bringup {

// Optional ESP-IDF/QEMU diagnostic. It keeps absolute emulator timing as
// regression-only telemetry and reports relative cost against a 16-Voice dry
// baseline so individual SF2 DSP features can be compared without conflating
// per-Voice work with the global chorus/reverb buses.
bool runOptionalDspStageTimingBenchmark();

}  // namespace gm_synth::bringup
