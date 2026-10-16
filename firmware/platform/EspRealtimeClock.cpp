#include "EspRealtimeClock.h"

#include "esp_timer.h"

#include <cstdint>

namespace gm_synth::platform {

std::uint64_t espRealtimeNowUs(void* context) {
    (void)context;
    const std::int64_t now = esp_timer_get_time();
    return now > 0 ? static_cast<std::uint64_t>(now) : 0u;
}

}  // namespace gm_synth::platform
