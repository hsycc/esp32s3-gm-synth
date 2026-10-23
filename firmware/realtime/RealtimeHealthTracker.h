#pragma once

#include <algorithm>
#include <cstdint>
#include <limits>

namespace gm_synth::realtime {

// Tracks the distinction between a soft one-block timing overrun and a hard
// producer backlog that cannot be recovered before the DMA safety window is
// consumed. This class is allocation-free and platform-independent so the
// qualification policy can be covered by host tests.
struct RealtimeHealthSnapshot {
    std::uint32_t block_budget_us = 0;
    std::uint32_t dma_window_us = 0;
    std::uint32_t blocks = 0;
    std::uint32_t render_soft_overruns = 0;
    std::uint32_t producer_work_soft_overruns = 0;
    std::uint32_t max_consecutive_work_overruns = 0;
    std::uint32_t max_backlog_recovery_blocks = 0;
    std::uint32_t max_backlog_blocks = 0;
    std::uint32_t min_dma_margin_us = 0;
    std::uint64_t total_producer_work_us = 0;
    std::uint64_t max_backlog_us = 0;
    std::uint64_t final_backlog_us = 0;
    bool backlog_exceeded_dma_window = false;

    std::uint32_t averageProducerWorkUs() const {
        return blocks == 0u
            ? 0u
            : static_cast<std::uint32_t>(total_producer_work_us / blocks);
    }

    bool backlogHealthy() const {
        return blocks != 0u &&
            !backlog_exceeded_dma_window &&
            final_backlog_us == 0u &&
            averageProducerWorkUs() <= block_budget_us;
    }
};

class RealtimeHealthTracker {
public:
    RealtimeHealthTracker(std::uint32_t block_budget_us, std::uint32_t dma_window_us)
        : block_budget_us_(block_budget_us),
          dma_window_us_(dma_window_us),
          min_dma_margin_us_(dma_window_us) {}

    void addBlock(std::uint32_t renderer_us, std::uint32_t producer_work_us) {
        if (block_budget_us_ == 0u) {
            return;
        }

        ++blocks_;
        total_producer_work_us_ += producer_work_us;
        if (renderer_us > block_budget_us_) {
            ++render_soft_overruns_;
        }

        const bool had_backlog = backlog_us_ != 0u;
        if (producer_work_us > block_budget_us_) {
            ++producer_work_soft_overruns_;
            ++consecutive_work_overruns_;
            max_consecutive_work_overruns_ = std::max(
                max_consecutive_work_overruns_,
                consecutive_work_overruns_);
            backlog_us_ += static_cast<std::uint64_t>(
                producer_work_us - block_budget_us_);
        } else {
            consecutive_work_overruns_ = 0u;
            const std::uint64_t recovery = block_budget_us_ - producer_work_us;
            backlog_us_ = recovery >= backlog_us_
                ? 0u
                : backlog_us_ - recovery;
        }

        if (!had_backlog && backlog_us_ != 0u) {
            current_recovery_blocks_ = 0u;
        } else if (had_backlog) {
            if (current_recovery_blocks_ != std::numeric_limits<std::uint32_t>::max()) {
                ++current_recovery_blocks_;
            }
            if (backlog_us_ == 0u) {
                max_backlog_recovery_blocks_ = std::max(
                    max_backlog_recovery_blocks_,
                    current_recovery_blocks_);
                current_recovery_blocks_ = 0u;
            }
        }

        max_backlog_us_ = std::max(max_backlog_us_, backlog_us_);
        if (dma_window_us_ != 0u && backlog_us_ >= dma_window_us_) {
            backlog_exceeded_dma_window_ = true;
        }

        const std::uint64_t margin = backlog_us_ >= dma_window_us_
            ? 0u
            : static_cast<std::uint64_t>(dma_window_us_) - backlog_us_;
        min_dma_margin_us_ = std::min<std::uint32_t>(
            min_dma_margin_us_,
            static_cast<std::uint32_t>(std::min<std::uint64_t>(
                margin,
                std::numeric_limits<std::uint32_t>::max())));
    }

    RealtimeHealthSnapshot snapshot() const {
        RealtimeHealthSnapshot result{};
        result.block_budget_us = block_budget_us_;
        result.dma_window_us = dma_window_us_;
        result.blocks = blocks_;
        result.render_soft_overruns = render_soft_overruns_;
        result.producer_work_soft_overruns = producer_work_soft_overruns_;
        result.max_consecutive_work_overruns = max_consecutive_work_overruns_;
        result.max_backlog_recovery_blocks = std::max(
            max_backlog_recovery_blocks_,
            current_recovery_blocks_);
        result.max_backlog_blocks = block_budget_us_ == 0u
            ? 0u
            : static_cast<std::uint32_t>(std::min<std::uint64_t>(
                (max_backlog_us_ + block_budget_us_ - 1u) / block_budget_us_,
                std::numeric_limits<std::uint32_t>::max()));
        result.min_dma_margin_us = min_dma_margin_us_;
        result.total_producer_work_us = total_producer_work_us_;
        result.max_backlog_us = max_backlog_us_;
        result.final_backlog_us = backlog_us_;
        result.backlog_exceeded_dma_window = backlog_exceeded_dma_window_;
        return result;
    }

private:
    std::uint32_t block_budget_us_ = 0;
    std::uint32_t dma_window_us_ = 0;
    std::uint32_t blocks_ = 0;
    std::uint32_t render_soft_overruns_ = 0;
    std::uint32_t producer_work_soft_overruns_ = 0;
    std::uint32_t consecutive_work_overruns_ = 0;
    std::uint32_t max_consecutive_work_overruns_ = 0;
    std::uint32_t current_recovery_blocks_ = 0;
    std::uint32_t max_backlog_recovery_blocks_ = 0;
    std::uint32_t min_dma_margin_us_ = 0;
    std::uint64_t total_producer_work_us_ = 0;
    std::uint64_t backlog_us_ = 0;
    std::uint64_t max_backlog_us_ = 0;
    bool backlog_exceeded_dma_window_ = false;
};

}  // namespace gm_synth::realtime
