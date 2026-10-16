#include "RenderDeadlineMonitor.h"

#include "util/SaturatingCounter.h"

#include <algorithm>
#include <cstdint>

namespace gm_synth::realtime {

std::size_t RenderDeadlineMonitor::observeBlock(
    std::uint32_t render_time_us,
    bool deadline_missed) {
    durations_us_[write_index_] = render_time_us;
    write_index_ = (write_index_ + 1u) % kWindowSize;
    if (window_count_ < kWindowSize) {
        ++window_count_;
    }

    util::saturatingIncrement(stats_.blocks_observed);
    stats_.last_render_us = render_time_us;
    stats_.peak_render_us = std::max(stats_.peak_render_us, render_time_us);
    stats_.window_count = window_count_;

    const bool effective_miss = deadline_missed ||
        render_time_us >= controller_.config().block_budget_us;
    if (effective_miss) {
        util::saturatingIncrement(stats_.deadline_misses);
    }

    return controller_.observeBlock(render_time_us, deadline_missed);
}

std::size_t RenderDeadlineMonitor::observeDeadlineMissWithoutTiming() {
    util::saturatingIncrement(stats_.blocks_observed);
    util::saturatingIncrement(stats_.deadline_misses);
    util::saturatingIncrement(stats_.untimed_deadline_misses);
    return controller_.observeBlock(0, true);
}

std::uint32_t RenderDeadlineMonitor::percentile(
    const std::array<std::uint32_t, kWindowSize>& sorted,
    std::size_t count,
    std::uint32_t percent) {
    if (count == 0) {
        return 0;
    }

    percent = std::clamp<std::uint32_t>(percent, 1u, 100u);
    const std::size_t rank =
        (static_cast<std::size_t>(percent) * count + 99u) / 100u;
    const std::size_t index = std::min(count - 1u, rank - 1u);
    return sorted[index];
}

RenderTimingSnapshot RenderDeadlineMonitor::snapshot() const {
    RenderTimingSnapshot result{};
    result.blocks_observed = stats_.blocks_observed;
    result.deadline_misses = stats_.deadline_misses;
    result.untimed_deadline_misses = stats_.untimed_deadline_misses;
    result.sample_count = window_count_;
    result.admission_limit = controller_.stats().current_limit;

    if (window_count_ == 0) {
        return result;
    }

    std::array<std::uint32_t, kWindowSize> sorted = durations_us_;
    std::sort(sorted.begin(), sorted.begin() + window_count_);

    std::uint64_t sum = 0;
    for (std::size_t index = 0; index < window_count_; ++index) {
        sum += sorted[index];
    }

    result.min_us = sorted[0];
    result.avg_us = static_cast<std::uint32_t>(sum / window_count_);
    result.p50_us = percentile(sorted, window_count_, 50);
    result.p95_us = percentile(sorted, window_count_, 95);
    result.p99_us = percentile(sorted, window_count_, 99);
    result.max_us = sorted[window_count_ - 1u];
    return result;
}

}  // namespace gm_synth::realtime
