#pragma once

#include "RenderDeadlineMonitor.h"
#include "ScheduledAudioBlockRenderer.h"

#include <cstddef>
#include <cstdint>

namespace gm_synth::realtime {

using RealtimeNowUsFn = std::uint64_t (*)(void* context);

class TimedRenderMeasurement {
public:
    TimedRenderMeasurement(const TimedRenderMeasurement&) = delete;
    TimedRenderMeasurement& operator=(const TimedRenderMeasurement&) = delete;
    TimedRenderMeasurement(TimedRenderMeasurement&&) = delete;
    TimedRenderMeasurement& operator=(TimedRenderMeasurement&&) = delete;

    bool timingValid() const { return timing_valid_; }
    std::uint32_t renderTimeUs() const { return render_time_us_; }

private:
    friend class TimedAudioBlockRenderer;

    TimedRenderMeasurement(bool timing_valid, std::uint32_t render_time_us)
        : timing_valid_(timing_valid),
          render_time_us_(render_time_us) {}

    bool timing_valid_ = false;
    std::uint32_t render_time_us_ = 0;
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
// SAME block observation instead of double-counting the block. Measurement
// tokens are renderer-created, caller-immutable and non-copyable/non-movable;
// observeMeasurement() consumes each token at most once. C++17 guaranteed copy
// elision lets renderMeasured() return the token without transferable aliases.
// render() keeps the one-shot behavior for benchmarks and render-only paths.
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
    ScheduledAudioBlockRenderer& renderer_;
    RenderDeadlineMonitor& monitor_;
    RealtimeNowUsFn now_us_ = nullptr;
    void* clock_context_ = nullptr;
    TimedAudioBlockStats stats_{};
};

}  // namespace gm_synth::realtime
