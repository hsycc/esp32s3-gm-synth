#include "TimedAudioBlockRenderer.h"

#include <cstdint>
#include <limits>

namespace gm_synth::realtime {

void TimedAudioBlockRenderer::saturatingIncrement(std::uint64_t& value) {
    if (value != std::numeric_limits<std::uint64_t>::max()) {
        ++value;
    }
}

std::size_t TimedAudioBlockRenderer::render(
    std::uint64_t block_start_frame,
    std::int16_t* interleaved_stereo,
    bool external_deadline_missed) {
    if (now_us_ == nullptr) {
        renderer_.render(block_start_frame, interleaved_stereo);
        saturatingIncrement(stats_.clock_faults);
        return external_deadline_missed
            ? monitor_.observeDeadlineMissWithoutTiming()
            : monitor_.currentAdmissionLimit();
    }

    const std::uint64_t start_us = now_us_(clock_context_);
    renderer_.render(block_start_frame, interleaved_stereo);
    const std::uint64_t end_us = now_us_(clock_context_);

    if (end_us < start_us) {
        saturatingIncrement(stats_.clock_faults);
        return external_deadline_missed
            ? monitor_.observeDeadlineMissWithoutTiming()
            : monitor_.currentAdmissionLimit();
    }

    std::uint64_t elapsed_us = end_us - start_us;
    if (elapsed_us > std::numeric_limits<std::uint32_t>::max()) {
        elapsed_us = std::numeric_limits<std::uint32_t>::max();
        saturatingIncrement(stats_.duration_clamps);
    }

    stats_.last_render_us = static_cast<std::uint32_t>(elapsed_us);
    saturatingIncrement(stats_.blocks_timed);
    return monitor_.observeBlock(stats_.last_render_us, external_deadline_missed);
}

}  // namespace gm_synth::realtime
