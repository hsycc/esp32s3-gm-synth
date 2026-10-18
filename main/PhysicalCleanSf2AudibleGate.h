#pragma once

namespace gm_synth::sample {
class SampleResidencyManager;
}

namespace gm_synth::bringup {

// Physical-only audible correctness gate. It resolves a real SF2 melodic
// program, guarantees its dependencies are resident before audio starts, then
// plays a sparse dry musical sequence through the production ZoneResolver /
// VoicePool / ScheduledAudioBlockRenderer / I2S path.
//
// This gate is deliberately separate from synthetic polyphony characterization:
// it must remain easy to judge by ear, and its realtime/atomic state stays in
// internal SRAM using split bounded allocations so it does not depend on one
// near-maximum contiguous internal-heap block.
bool runPhysicalCleanSf2AudibleGate(sample::SampleResidencyManager& residency);

}  // namespace gm_synth::bringup
