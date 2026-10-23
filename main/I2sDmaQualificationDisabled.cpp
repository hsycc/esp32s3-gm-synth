#include "I2sDmaQualification.h"

namespace gm_synth::bringup {

bool runOptionalI2sDmaQualification(
    realtime::ScheduledAudioBlockRenderer& renderer,
    std::uint64_t block_start_frame) {
    (void)renderer;
    (void)block_start_frame;
    return true;
}

}  // namespace gm_synth::bringup
