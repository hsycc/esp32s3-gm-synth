#pragma once

namespace gm_synth::sample {
class SampleResidencyManager;
}

namespace gm_synth::bringup {

// Physical-only audible correctness gate. It resolves a real SF2 melodic
// program, guarantees its dependencies are resident before audio starts, then
// plays a sparse dry musical sequence through the production ZoneResolver /
// VoicePool / ScheduledAudioBlockRenderer / I2S path.
bool runPhysicalCleanSf2AudibleGate(sample::SampleResidencyManager& residency);

}  // namespace gm_synth::bringup
