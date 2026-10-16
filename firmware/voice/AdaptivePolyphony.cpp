#include "AdaptivePolyphony.h"

#include "util/SaturatingCounter.h"

#include <algorithm>
#include <cstdint>

namespace gm_synth::voice {

AdaptivePolyphonyController::AdaptivePolyphonyController(
    VoicePool& voices,
    AdaptivePolyphonyConfig config)
    : voices_(voices), config_(config) {
    config_.block_budget_us = std::max<std::uint32_t>(1, config_.block_budget_us);
    config_.high_pressure_percent = std::clamp<std::uint8_t>(
        config_.high_pressure_percent, 1, 100);
    config_.recovery_pressure_percent = std::clamp<std::uint8_t>(
        config_.recovery_pressure_percent, 1, config_.high_pressure_percent);
    config_.high_pressure_blocks = std::max<std::uint16_t>(1, config_.high_pressure_blocks);
    config_.recovery_blocks = std::max<std::uint16_t>(1, config_.recovery_blocks);

    config_.min_limit = std::clamp<std::size_t>(
        config_.min_limit, 1, VoicePool::kHardVoiceCap);
    config_.max_limit = std::clamp<std::size_t>(
        config_.max_limit, config_.min_limit, VoicePool::kHardVoiceCap);
    config_.initial_limit = std::clamp<std::size_t>(
        config_.initial_limit, config_.min_limit, config_.max_limit);

    voices_.setAdmissionLimit(config_.initial_limit);
    stats_.current_limit = voices_.admissionLimit();
    stats_.minimum_limit_observed = stats_.current_limit;
    stats_.maximum_limit_observed = stats_.current_limit;
}

bool AdaptivePolyphonyController::atOrAbovePercent(
    std::uint32_t render_time_us,
    std::uint8_t percent) const {
    return static_cast<std::uint64_t>(render_time_us) * 100u >=
        static_cast<std::uint64_t>(config_.block_budget_us) * percent;
}

bool AdaptivePolyphonyController::atOrBelowPercent(
    std::uint32_t render_time_us,
    std::uint8_t percent) const {
    return static_cast<std::uint64_t>(render_time_us) * 100u <=
        static_cast<std::uint64_t>(config_.block_budget_us) * percent;
}

void AdaptivePolyphonyController::reduceOne() {
    if (stats_.current_limit <= config_.min_limit) {
        return;
    }
    voices_.setAdmissionLimit(stats_.current_limit - 1u);
    stats_.current_limit = voices_.admissionLimit();
    stats_.minimum_limit_observed = std::min(
        stats_.minimum_limit_observed, stats_.current_limit);
    util::saturatingIncrement(stats_.limit_reductions);
}

void AdaptivePolyphonyController::reduceForDeadlineMiss() {
    if (stats_.current_limit <= config_.min_limit) {
        return;
    }

    const std::size_t decrement = std::max<std::size_t>(1, stats_.current_limit / 4u);
    const std::size_t target = stats_.current_limit > decrement
        ? stats_.current_limit - decrement
        : config_.min_limit;
    voices_.setAdmissionLimit(std::max(config_.min_limit, target));
    stats_.current_limit = voices_.admissionLimit();
    stats_.minimum_limit_observed = std::min(
        stats_.minimum_limit_observed, stats_.current_limit);
    util::saturatingIncrement(stats_.limit_reductions);
}

void AdaptivePolyphonyController::recoverOne() {
    if (stats_.current_limit >= config_.max_limit) {
        return;
    }
    voices_.setAdmissionLimit(stats_.current_limit + 1u);
    stats_.current_limit = voices_.admissionLimit();
    stats_.maximum_limit_observed = std::max(
        stats_.maximum_limit_observed, stats_.current_limit);
    util::saturatingIncrement(stats_.limit_recoveries);
}

std::size_t AdaptivePolyphonyController::observeBlock(
    std::uint32_t render_time_us,
    bool deadline_missed) {
    util::saturatingIncrement(stats_.blocks_observed);
    stats_.peak_render_us = std::max(stats_.peak_render_us, render_time_us);

    const bool missed = deadline_missed || render_time_us >= config_.block_budget_us;
    if (missed) {
        util::saturatingIncrement(stats_.deadline_misses);
        util::saturatingIncrement(stats_.high_pressure_events);
        high_pressure_streak_ = 0;
        recovery_streak_ = 0;
        reduceForDeadlineMiss();
        return stats_.current_limit;
    }

    if (atOrAbovePercent(render_time_us, config_.high_pressure_percent)) {
        recovery_streak_ = 0;
        if (high_pressure_streak_ < config_.high_pressure_blocks) {
            ++high_pressure_streak_;
        }
        if (high_pressure_streak_ >= config_.high_pressure_blocks) {
            util::saturatingIncrement(stats_.high_pressure_events);
            reduceOne();
            high_pressure_streak_ = 0;
        }
        return stats_.current_limit;
    }

    if (atOrBelowPercent(render_time_us, config_.recovery_pressure_percent)) {
        high_pressure_streak_ = 0;
        if (recovery_streak_ < config_.recovery_blocks) {
            ++recovery_streak_;
        }
        if (recovery_streak_ >= config_.recovery_blocks) {
            recoverOne();
            recovery_streak_ = 0;
        }
        return stats_.current_limit;
    }

    high_pressure_streak_ = 0;
    recovery_streak_ = 0;
    return stats_.current_limit;
}

}  // namespace gm_synth::voice
