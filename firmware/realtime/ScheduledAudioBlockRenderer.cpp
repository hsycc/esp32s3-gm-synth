#include "ScheduledAudioBlockRenderer.h"

#include <algorithm>
#include <cstdint>
#include <limits>

namespace gm_synth::realtime {

std::int16_t ScheduledAudioBlockRenderer::saturateToI16(
    std::int32_t sample,
    bool* clipped) {
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

void ScheduledAudioBlockRenderer::renderSegment(
    std::size_t start_frame,
    std::size_t frame_count) {
    if (frame_count == 0) {
        return;
    }
    voices_.renderAdd(mix_.data() + (start_frame * kChannels), frame_count);
    ++stats_.render_segments;
}

void ScheduledAudioBlockRenderer::finalize(std::int16_t* interleaved_stereo) {
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
}

void ScheduledAudioBlockRenderer::render(
    std::uint64_t block_start_frame,
    std::int16_t* interleaved_stereo) {
    if (interleaved_stereo == nullptr) {
        return;
    }

    mix_.fill(0);
    const BlockEventBatch& batch = scheduler_.prepareBlock(
        block_start_frame,
        static_cast<std::uint16_t>(kFramesPerBlock));

    std::size_t cursor = 0;
    std::size_t event_index = 0;
    while (event_index < batch.count) {
        const std::size_t offset = std::min<std::size_t>(
            batch.events[event_index].frame_offset,
            kFramesPerBlock);
        if (offset > cursor) {
            renderSegment(cursor, offset - cursor);
            cursor = offset;
        }

        // Apply all equal-offset events before rendering the frame at that
        // offset. FIFO is inherited from the SPSC queue and scheduler batch.
        while (event_index < batch.count &&
               batch.events[event_index].frame_offset == offset) {
            ++stats_.events_attempted;
            const bool applied = apply_event_ != nullptr &&
                apply_event_(apply_context_, batch.events[event_index].event, voices_);
            if (applied) {
                ++stats_.events_applied;
            } else {
                ++stats_.events_rejected;
            }
            ++event_index;
        }
    }

    if (cursor < kFramesPerBlock) {
        renderSegment(cursor, kFramesPerBlock - cursor);
    }

    finalize(interleaved_stereo);
    ++stats_.blocks_rendered;
    stats_.output_frames += kFramesPerBlock;
}

}  // namespace gm_synth::realtime
