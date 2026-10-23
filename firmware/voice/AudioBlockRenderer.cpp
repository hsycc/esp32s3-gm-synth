#include "AudioBlockRenderer.h"

#include <algorithm>
#include <cstdint>
#include <limits>

namespace gm_synth::voice {

std::int16_t AudioBlockRenderer::saturateToI16(std::int32_t sample, bool* clipped) {
    if (sample > std::numeric_limits<std::int16_t>::max()) {
        if (clipped != nullptr) {
            *clipped = true;
        }
        return std::numeric_limits<std::int16_t>::max();
    }
    if (sample < std::numeric_limits<std::int16_t>::min()) {
        if (clipped != nullptr) {
            *clipped = true;
        }
        return std::numeric_limits<std::int16_t>::min();
    }
    return static_cast<std::int16_t>(sample);
}

void AudioBlockRenderer::render(std::int16_t* interleaved_stereo) {
    if (interleaved_stereo == nullptr) {
        return;
    }

    mix_.fill(0);
    reverb_send_.fill(0);
    chorus_send_.fill(0);
    voices_.renderAddWithEffects(
        mix_.data(),
        reverb_send_.data(),
        chorus_send_.data(),
        kFramesPerBlock);
    effects_.processAdd(
        chorus_send_.data(),
        reverb_send_.data(),
        mix_.data(),
        kFramesPerBlock);

    for (std::size_t index = 0; index < mix_.size(); ++index) {
        const std::int32_t value = mix_[index];
        const std::int64_t magnitude = value >= 0
            ? static_cast<std::int64_t>(value)
            : -static_cast<std::int64_t>(value);
        stats_.peak_abs_accumulator = std::max(
            stats_.peak_abs_accumulator,
            static_cast<std::int32_t>(std::min<std::int64_t>(
                magnitude,
                std::numeric_limits<std::int32_t>::max())));

        bool clipped = false;
        interleaved_stereo[index] = saturateToI16(value, &clipped);
        if (clipped) {
            ++stats_.clipped_samples;
        }
    }

    ++stats_.blocks_rendered;
    stats_.output_frames += kFramesPerBlock;
}

}  // namespace gm_synth::voice
