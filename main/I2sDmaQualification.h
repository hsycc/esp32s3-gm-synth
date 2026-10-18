#pragma once

#include <cstdint>

namespace gm_synth::realtime {
class ScheduledAudioBlockRenderer;
}

namespace gm_synth::bringup {

// Runs the optional physical I2S/DMA qualification against an already-active
// deterministic realtime renderer. The feature is enabled only by the
// dedicated Kconfig switch; normal host/QEMU/bring-up builds never start I2S.
bool runOptionalI2sDmaQualification(
    realtime::ScheduledAudioBlockRenderer& renderer,
    std::uint64_t block_start_frame);

}  // namespace gm_synth::bringup
