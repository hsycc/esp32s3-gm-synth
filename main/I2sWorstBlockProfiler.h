#pragma once

#include "realtime/ScheduledAudioBlockRenderer.h"

namespace gm_synth::bringup {

// Emit the retained renderer profile only after the continuous-I2S producer
// window has stopped. Keeping logging out of the active path avoids turning
// diagnostics into a new realtime deadline source.
void logI2sWorstBlockProfile(
    const realtime::ScheduledWorstBlockProfile& profile);

}  // namespace gm_synth::bringup
