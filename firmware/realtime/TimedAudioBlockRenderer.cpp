#include "TimedAudioBlockRenderer.h"

#include <cstdint>
#include <limits>

namespace gm_synth::realtime {

void TimedAudioBlockRenderer::saturatingIncrement(std::uint64_t& value) {
    if (value != std::numeric_limits<std::uint64_t>::max()) {
        ++value;
    }
}

TimedRenderMeasurement TimedAudioBlockRenderer::renderMeasured(
    std::uint64_t block_start_frame,
    std::int16_t* interleaved_stereo) {
    TimedRenderMeasurement measurement{};

    if (now_us_ == nullptr) {
        renderer_.render(block_start_frame, interleaved_stereo);
        saturatingIncrement(stats_.clock_faults);
        return measurement;
    }

    const std::uint64_t start_us = now_us_(clock_context_);
    renderer_.render(block_start_frame, interleaved_stereo);
    const std::uint64_t end_us = now_us_(clock_context_);

    if (end_us < start_us) {
        saturatingIncrement(stats_.clock_faults);
        return measurement;
    }

    std::uint64_t elapsed_us = end_us - start_us;
    if (elapsed_us > std::numeric_limits<std::uint32_t>::max()) {
        elapsed_us = std::numeric_limits<std::uint32_t>::max();
        saturatingIncrement(stats_.duration_clamps);
    }

    measurement.timing_valid = true;
    measurement.render_time_us = static_cast<std::uint32_t>(elapsed_us);
    stats_.last_render_us = measurement.render_time_us;
    saturatingIncrement(stats_.blocks_timed);
    return measurement;
}

std::size_t TimedAudioBlockRenderer::observeMeasurement(
    TimedRenderMeasurement& measurement,
    bool external_deadline_missed) {
    if (measurement.observed_) {
        saturatingIncrement(stats_.duplicate_observations);
        return monitor_.currentAdmissionLimit();
    }
    measurement.observed_ = true;

    if (measurement.timing_valid) {
        return monitor_.observeBlock(
            measurement.render_time_us,
            external_deadline_missed);
    }
    return external_deadline_missed
        ? monitor_.observeDeadlineMissWithoutTiming()
        : monitor_.currentAdmissionLimit();
}

std::size_t TimedAudioBlockRenderer::render(
    std::uint64_t block_start_frame,
    std::int16_t* interleaved_stereo,
    bool external_deadline_missed) {
    TimedRenderMeasurement measurement =
        renderMeasured(block_start_frame, interleaved_stereo);
    return observeMeasurement(measurement, external_deadline_missed);
}

}  // namespace gm_synth::realtime
