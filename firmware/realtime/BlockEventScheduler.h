#pragma once

#include "RealtimeEventQueue.h"

#include <array>
#include <cstddef>
#include <cstdint>

namespace gm_synth::realtime {

struct BlockEvent {
    RealtimeEvent event{};
    std::uint16_t frame_offset = 0;
};

struct BlockEventBatch {
    std::array<BlockEvent, RealtimeEventQueue::kCapacity> events{};
    std::size_t count = 0;
};

struct BlockEventSchedulerStats {
    std::uint64_t blocks_prepared = 0;
    std::uint64_t events_dispatched = 0;
    std::uint64_t late_events = 0;
    std::uint64_t empty_blocks = 0;
};

// Audio-side consumer for the SPSC event queue. It converts absolute frame
// timestamps into block-local offsets without allocation, sorting or blocking.
class BlockEventScheduler {
public:
    explicit BlockEventScheduler(RealtimeEventQueue& queue)
        : queue_(queue) {}

    // Drains all events whose timestamp is strictly before block_start +
    // block_frames. Events exactly on the end boundary remain queued for the
    // next block and become offset zero there. Late events clamp to offset zero.
    const BlockEventBatch& prepareBlock(
        std::uint64_t block_start_frame,
        std::uint16_t block_frames);

    const BlockEventSchedulerStats& stats() const { return stats_; }

private:
    RealtimeEventQueue& queue_;
    BlockEventBatch batch_{};
    BlockEventSchedulerStats stats_{};
};

}  // namespace gm_synth::realtime
