#pragma once

#include "voice/AdaptivePolyphony.h"

#include <array>
#include <cstddef>
#include <cstdint>

namespace gm_synth::realtime {

struct RenderDeadlineStats {
    std::uint64_t blocks_observed = 0;
    std::uint64_t deadline_misses = 0;
    std::uint32_t last_render_us = 0;
    std::uint32_t peak_render_us = 0;
    std::size_t window_count = 0;
};

struct RenderTimingSnapshot {
    std::uint64_t blocks_observed = 0;
    std::uint64_t deadline_misses = 0;
    std::size_t sample_count = 0;
    std::uint32_t min_us = 0;
    std::uint32_t avg_us = 0;
    std::uint32_t p50_us = 0;
    std::uint32_t p95_us = 0;
    std::uint32_t p99_us = 0;
    std::uint32_t max_us = 0;
    std::size_t admission_limit = 0;
};

// O(1) audio-side render timing recorder feeding AdaptivePolyphonyController.
// No allocation, lock, sorting or I/O occurs in observeBlock(). snapshot() is
// intended for the control/reporting domain and uses only fixed stack storage.
//
// QEMU timing is useful as a regression baseline only. Physical N16R8 timing
// remains authoritative for CPU headroom, I2S deadlines and stable polyphony.
class RenderDeadlineMonitor {
public:
    static constexpr std::size_t kWindowSize = 128;

    explicit RenderDeadlineMonitor(voice::AdaptivePolyphonyController& controller)
        : controller_(controller) {}

    std::size_t observeBlock(
        std::uint32_t render_time_us,
        bool deadline_missed = false);

    RenderTimingSnapshot snapshot() const;
    const RenderDeadlineStats& stats() const { return stats_; }

private:
    static std::uint32_t percentile(
        const std::array<std::uint32_t, kWindowSize>& sorted,
        std::size_t count,
        std::uint32_t percent);
    static void saturatingIncrement(std::uint64_t& value);

    voice::AdaptivePolyphonyController& controller_;
    std::array<std::uint32_t, kWindowSize> durations_us_{};
    std::size_t write_index_ = 0;
    std::size_t window_count_ = 0;
    RenderDeadlineStats stats_{};
};

}  // namespace gm_synth::realtime
