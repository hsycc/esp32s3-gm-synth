#pragma once

#include "VoiceRenderer.h"

#include <cstddef>
#include <cstdint>

namespace gm_synth::voice {

struct AdaptivePolyphonyConfig {
    // 64 frames / 44.1 kHz ~= 1451 us. This is a scheduler deadline, not a
    // claimed ESP32-S3 performance result; physical qualification tunes limits.
    std::uint32_t block_budget_us = 1451;
    std::uint8_t high_pressure_percent = 85;
    std::uint8_t recovery_pressure_percent = 60;
    std::uint16_t high_pressure_blocks = 4;
    std::uint16_t recovery_blocks = 128;
    std::size_t min_limit = 4;
    std::size_t initial_limit = 16;
    std::size_t max_limit = 16;
};

struct AdaptivePolyphonyStats {
    std::uint64_t blocks_observed = 0;
    std::uint64_t deadline_misses = 0;
    std::uint64_t high_pressure_events = 0;
    std::uint64_t limit_reductions = 0;
    std::uint64_t limit_recoveries = 0;
    std::uint32_t peak_render_us = 0;
    std::size_t current_limit = 0;
    std::size_t minimum_limit_observed = 0;
    std::size_t maximum_limit_observed = 0;
};

// Deterministic admission controller. It never steals or terminates an active
// Voice. It only adjusts how many new voices VoicePool will admit. M6 Phase B
// can add explicit fast-release victim selection without changing this policy.
class AdaptivePolyphonyController {
public:
    explicit AdaptivePolyphonyController(
        VoicePool& voices,
        AdaptivePolyphonyConfig config = {});

    std::size_t observeBlock(
        std::uint32_t render_time_us,
        bool deadline_missed = false);

    const AdaptivePolyphonyConfig& config() const { return config_; }
    const AdaptivePolyphonyStats& stats() const { return stats_; }

private:
    bool atOrAbovePercent(std::uint32_t render_time_us, std::uint8_t percent) const;
    bool atOrBelowPercent(std::uint32_t render_time_us, std::uint8_t percent) const;
    void reduceForDeadlineMiss();
    void reduceOne();
    void recoverOne();

    VoicePool& voices_;
    AdaptivePolyphonyConfig config_{};
    AdaptivePolyphonyStats stats_{};
    std::uint16_t high_pressure_streak_ = 0;
    std::uint16_t recovery_streak_ = 0;
};

}  // namespace gm_synth::voice
