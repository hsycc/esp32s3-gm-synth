#pragma once

#include "util/SaturatingCounter.h"
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>

namespace gm_synth::app {

// Experimental digital sample-peak protection, NOT true-peak reconstruction,
// analogue/DAC protection or an audible-quality approval. One audio owner.
// The mode is immutable for a stream; enabling it adds exactly 64 output frames
// of latency. It does not change Voice admission, MIDI gain or the FX sends.
class OutputPeakProtector {
public:
    enum class Mode : std::uint8_t { Bypass, Peak64 };
    static constexpr std::size_t kFrames = 64;
    static constexpr std::size_t kSamples = kFrames * 2;
    static constexpr std::uint32_t kUnityQ30 = std::uint32_t{1} << 30;
    static constexpr std::uint32_t kReleaseStepQ30 = kUnityQ30 / 64;
    static constexpr std::uint32_t kCeiling = 32767;

    struct Stats {
        std::uint64_t input_frames = 0, output_frames = 0;
        // Post-master, pre-protection: these remain observable even when the
        // protected output has no hard clipping. INT32_MIN has magnitude 2^31.
        std::uint32_t raw_peak_abs = 0;
        std::uint64_t raw_overload_samples = 0;
        std::uint32_t post_peak_abs = 0;
        std::uint64_t post_hard_clipped = 0, limited_frames = 0;
        std::uint32_t gain_min_q30 = kUnityQ30;
        std::uint32_t latency_frames = 0;
    };

    explicit OutputPeakProtector(Mode mode = Mode::Bypass) noexcept : mode_(mode) {
        stats_.latency_frames = mode == Mode::Peak64 ? kFrames : 0;
    }
    OutputPeakProtector(const OutputPeakProtector&) = delete;
    OutputPeakProtector& operator=(const OutputPeakProtector&) = delete;

    Mode mode() const noexcept { return mode_; }
    const Stats& stats() const noexcept { return stats_; }
    std::uint32_t gainQ30() const noexcept { return gain_q30_; }

    // Input/output buffers must not overlap. Only complete stereo 64-frame
    // blocks and a non-amplifying Q15 master are accepted. Invalid calls leave
    // both state and output untouched. No allocation, I/O, clock or locks.
    bool process(const std::int32_t* input, std::size_t frames,
                 std::int32_t master_gain_q15, std::int16_t* output) noexcept {
        if (!input || !output || frames != kFrames || master_gain_q15 < 0 ||
            master_gain_q15 > 32767 || finished_ ||
            (mode_ != Mode::Bypass && mode_ != Mode::Peak64)) return false;
        std::uint32_t peak = 0;
        for (std::size_t i = 0; i < kSamples; ++i) {
            const auto value = master(input[i], master_gain_q15);
            peak = std::max(peak, magnitude(value));
            if (outsidePcm(value)) util::saturatingIncrement(stats_.raw_overload_samples);
            if (mode_ == Mode::Bypass) output[i] = convert(value);
        }
        stats_.raw_peak_abs = std::max(stats_.raw_peak_abs, peak);
        util::saturatingAdd(stats_.input_frames, std::uint64_t{kFrames});
        if (mode_ == Mode::Bypass) {
            util::saturatingAdd(stats_.output_frames, std::uint64_t{kFrames});
            return true;
        }

        const std::uint32_t cap = peak <= kCeiling ? kUnityQ30 :
            static_cast<std::uint32_t>((std::uint64_t{kCeiling} * kUnityQ30) / peak);
        if (pending_) {
            emit(cap, output);
        } else {
            // Prime the delay. The first audible sample will already have a
            // safe gain even when the stream begins with an overloaded impulse.
            std::fill_n(output, kSamples, std::int16_t{0});
            gain_q30_ = std::min(gain_q30_, cap);
            util::saturatingAdd(stats_.output_frames, std::uint64_t{kFrames});
        }
        for (std::size_t i = 0; i < kSamples; ++i)
            delay_[i] = master(input[i], master_gain_q15);
        pending_cap_q30_ = cap;
        pending_ = true;
        return true;
    }

    // The owner must write this final block to the sink before stopping. It is
    // not another synthesized block: raw input statistics remain unchanged.
    // The terminal flush is one-shot; subsequent process/flush calls fail.
    bool flush(std::int16_t* output) noexcept {
        if (!output || mode_ != Mode::Peak64 || !pending_ || finished_) return false;
        emit(kUnityQ30, output);
        pending_ = false;
        finished_ = true;
        return true;
    }

private:
    static std::int32_t master(std::int32_t value, std::int32_t gain) noexcept {
        return gain == 32767 ? value : static_cast<std::int32_t>(
            (static_cast<std::int64_t>(value) * gain) / 32767);
    }
    static std::uint32_t magnitude(std::int32_t value) noexcept {
        return static_cast<std::uint32_t>(value >= 0 ?
            static_cast<std::int64_t>(value) : -static_cast<std::int64_t>(value));
    }
    static bool outsidePcm(std::int32_t value) noexcept { return value > 32767 || value < -32768; }
    std::int16_t convert(std::int32_t value) noexcept {
        stats_.post_peak_abs = std::max(stats_.post_peak_abs, magnitude(value));
        if (outsidePcm(value)) util::saturatingIncrement(stats_.post_hard_clipped);
        return static_cast<std::int16_t>(std::clamp(value, std::int32_t{-32768}, std::int32_t{32767}));
    }

    void emit(std::uint32_t next_cap, std::int16_t* output) noexcept {
        // Induction: the preceding block ended at a gain <= this pending cap.
        // Both endpoints below are <= pending cap, so every interpolated gain
        // bounds BOTH channels, including negative full-range int32 input.
        // Lookahead allows attack to ramp before the next block's peak. Release
        // is linear, at most unity/64 per block: 4096 frames from zero to unity.
        const auto end = std::min({pending_cap_q30_, next_cap,
            std::min(kUnityQ30, gain_q30_ + kReleaseStepQ30)});
        const auto delta = static_cast<std::int32_t>(end) - static_cast<std::int32_t>(gain_q30_);
        const auto step = delta / static_cast<std::int32_t>(kFrames);
        const auto remainder = delta % static_cast<std::int32_t>(kFrames);
        std::int32_t error = 0;
        auto gain = static_cast<std::int32_t>(gain_q30_);
        for (std::size_t frame = 0; frame < kFrames; ++frame) {
            gain += step;
            error += remainder;
            if (error >= static_cast<std::int32_t>(kFrames)) { ++gain; error -= static_cast<std::int32_t>(kFrames); }
            if (error <= -static_cast<std::int32_t>(kFrames)) { --gain; error += static_cast<std::int32_t>(kFrames); }
            if (static_cast<std::uint32_t>(gain) < kUnityQ30) util::saturatingIncrement(stats_.limited_frames);
            stats_.gain_min_q30 = std::min(stats_.gain_min_q30, static_cast<std::uint32_t>(gain));
            for (std::size_t channel = 0; channel < 2; ++channel) {
                const auto i = frame * 2 + channel;
                // Division by constant 2^30 has defined rounding toward zero;
                // do not rely on implementation-defined signed right shift.
                const auto scaled = static_cast<std::int32_t>(
                    (static_cast<std::int64_t>(delay_[i]) * gain) / kUnityQ30);
                output[i] = convert(scaled);
            }
        }
        gain_q30_ = end;
        util::saturatingAdd(stats_.output_frames, std::uint64_t{kFrames});
    }

    const Mode mode_;
    Stats stats_{};
    std::array<std::int32_t, kSamples> delay_{};
    std::uint32_t gain_q30_ = kUnityQ30, pending_cap_q30_ = kUnityQ30;
    bool pending_ = false, finished_ = false;
};

} // namespace gm_synth::app
