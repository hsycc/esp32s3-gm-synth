#pragma once

#include <cstdint>

namespace gm_synth::platform {

// Adapter matching realtime::RealtimeNowUsFn. Context is unused.
std::uint64_t espRealtimeNowUs(void* context);

}  // namespace gm_synth::platform
