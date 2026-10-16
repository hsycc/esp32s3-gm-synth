#include "BlockEventScheduler.h"

#include <limits>

namespace gm_synth::realtime {

const BlockEventBatch& BlockEventScheduler::prepareBlock(
    std::uint64_t block_start_frame,
    std::uint16_t block_frames) {
    batch_.count = 0;
    ++stats_.blocks_prepared;

    if (block_frames == 0) {
        ++stats_.empty_blocks;
        return batch_;
    }

    const std::uint64_t remaining =
        std::numeric_limits<std::uint64_t>::max() - block_start_frame;
    const bool end_overflow = block_frames > remaining;
    const std::uint64_t block_end = end_overflow
        ? std::numeric_limits<std::uint64_t>::max()
        : block_start_frame + block_frames;

    while (batch_.count < batch_.events.size()) {
        RealtimeEvent next{};
        if (!queue_.tryPeek(&next)) {
            break;
        }

        const bool due = end_overflow ? true : next.timestamp_frame < block_end;
        if (!due) {
            break;
        }

        RealtimeEvent popped{};
        if (!queue_.tryPop(&popped)) {
            break;
        }

        BlockEvent& destination = batch_.events[batch_.count++];
        destination.event = popped;
        if (popped.timestamp_frame <= block_start_frame) {
            destination.frame_offset = 0;
            if (popped.timestamp_frame < block_start_frame) {
                ++stats_.late_events;
            }
        } else {
            destination.frame_offset = static_cast<std::uint16_t>(
                popped.timestamp_frame - block_start_frame);
        }
        ++stats_.events_dispatched;
    }

    if (batch_.count == 0) {
        ++stats_.empty_blocks;
    }
    return batch_;
}

}  // namespace gm_synth::realtime
