#pragma once

#include "RenderDeadlineMonitor.h"
#include "ScheduledAudioBlockRenderer.h"

#include <cstddef>
#include <cstdint>

namespace gm_synth::realtime {

using RealtimeNowUsFn = std::uint64_t (*)(void* context);

struct TimedRenderMeasurement {
    bool timing_valid = false;
    std::uint32_t render_time_us = 0;

    TimedRenderMeasurement() = default;
    TimedRenderMeasurement(const TimedRenderMeasurement&) = delete;
    TimedRenderMeasurement& operator=(const TimedRenderMeasurement&) = delete;
    TimedRenderMeasurement(TimedRenderMeasurement&&) = default;
    TimedRenderMeasurement& operator=(TimedRenderMeasurement&&) = default;

private:
    friend class TimedAudioBlockRenderer;
    bool observed_ = false;
};

struct TimedAudioBlockStats {
    std::uint64_t blocks_timed = 0;
    std::uint64_t clock_faults = 0;
    std::uint64_t duration_clamps = 0;
    std::uint64_t duplicate_observations = 0;
    std::uint32_t last_render_us = 0;
};

// Measures one complete ScheduledAudioBlockRenderer::render() call with an
// injected monotonic microsecond clock. The injection keeps Host/QEMU tests
// deterministic while the ESP32-S3 adapter can use esp_timer_get_time().
//
// renderMeasured() deliberately separates measurement from policy observation.
// That lets a later I2S write contribute its short-write/timeout result to the
// SAME block observation instead of double-counting the block. Measurements are
// move-only and observeMeasurement() consumes each measurement at most once.
// render() keeps the convenient one-shot behavior for benchmarks/render-only paths.
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

    TimedRenderMeasurement renderMeasured(
        std::uint64_t block_start_frame,
        std::int16_t* interleaved_stereo);

    std::size_t observeMeasurement(
        TimedRenderMeasurement& measurement,
        bool external_deadline_missed = false);

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
