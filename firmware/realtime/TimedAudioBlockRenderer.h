#pragma once

#include "RenderDeadlineMonitor.h"
#include "ScheduledAudioBlockRenderer.h"

#include <cstddef>
#include <cstdint>

namespace gm_synth::realtime {

using RealtimeNowUsFn = std::uint64_t (*)(void* context);

struct TimedAudioBlockStats {
    std::uint64_t blocks_timed = 0;
    std::uint64_t clock_faults = 0;
    std::uint64_t duration_clamps = 0;
    std::uint32_t last_render_us = 0;
};

// Measures one complete ScheduledAudioBlockRenderer::render() call with an
// injected monotonic microsecond clock. The injection keeps Host/QEMU tests
// deterministic while the ESP32-S3 adapter can use esp_timer_get_time().
//
// If the clock is missing or moves backwards, rendering still completes but
// the invalid measurement is NOT fed into adaptive admission.
class TimedAudioBlockRenderer {
public:
    TimedAudioBlockRenderer(
        ScheduledAudioBlockRenderer& renderer,
        RenderDeadlineMonitor& monitor,
        RealtimeNowUsFn now_us,
        void* clock_context = nullptr)
        : renderer_(renderer),
          monitor_(monitor),
          now_us_(now_us),
          clock_context_(clock_context) {}

    std::size_t render(
        std::uint64_t block_start_frame,
        std::int16_t* interleaved_stereo,
        bool external_deadline_missed = false);

    const TimedAudioBlockStats& stats() const { return stats_; }

private:
    static void saturatingIncrement(std::uint64_t& value);

    ScheduledAudioBlockRenderer& renderer_;
    RenderDeadlineMonitor& monitor_;
    RealtimeNowUsFn now_us_ = nullptr;
    void* clock_context_ = nullptr;
    TimedAudioBlockStats stats_{};
};

}  // namespace gm_synth::realtime
