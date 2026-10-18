#include "GlobalEffectsProcessor.h"

#include <algorithm>
#include <cstdint>
#include <limits>

namespace gm_synth::voice {
namespace {

constexpr std::int32_t kQ15Unity = 32767;
constexpr std::int32_t kChorusWetGainQ15 = 12000;
constexpr std::int32_t kReverbWetGainQ15 = 11000;
constexpr std::int32_t kReverbFeedbackAQ15 = 23800;
constexpr std::int32_t kReverbFeedbackBQ15 = 23100;
constexpr std::int32_t kReverbFeedbackCQ15 = 22400;
constexpr std::size_t kChorusBaseDelayFrames = 600;
constexpr std::size_t kChorusDepthFrames = 96;
// ~0.65 Hz at 44.1 kHz in Q0.32 cycles/frame.
constexpr std::uint32_t kChorusPhaseStepQ32 = 63300u;

inline void advanceRingIndex(std::size_t* index, std::size_t size) {
    ++(*index);
    if (*index == size) {
        *index = 0;
    }
}

}  // namespace

void GlobalEffectsProcessor::reset() {
    chorus_delay_.fill(0);
    reverb_comb_a_.fill(0);
    reverb_comb_b_.fill(0);
    reverb_comb_c_.fill(0);
    reverb_allpass_l_.fill(0);
    reverb_allpass_r_.fill(0);
    chorus_write_ = 0;
    chorus_phase_q32_ = 0;
    reverb_comb_a_index_ = 0;
    reverb_comb_b_index_ = 0;
    reverb_comb_c_index_ = 0;
    reverb_allpass_l_index_ = 0;
    reverb_allpass_r_index_ = 0;
}

std::int16_t GlobalEffectsProcessor::clampI16(std::int32_t value) {
    return static_cast<std::int16_t>(std::clamp<std::int32_t>(
        value,
        std::numeric_limits<std::int16_t>::min(),
        std::numeric_limits<std::int16_t>::max()));
}

std::int32_t GlobalEffectsProcessor::scaleQ15(
    std::int32_t value,
    std::int32_t gain_q15) {
    if (value == 0 || gain_q15 == 0) {
        return 0;
    }
    const std::int64_t product = static_cast<std::int64_t>(value) * gain_q15;
    return product >= 0
        ? static_cast<std::int32_t>((product + (std::int64_t{1} << 14)) >> 15)
        : -static_cast<std::int32_t>(((-product) + (std::int64_t{1} << 14)) >> 15);
}

std::int32_t GlobalEffectsProcessor::averageStereo(
    const std::int32_t* bus,
    std::size_t frame) {
    if (bus == nullptr) {
        return 0;
    }
    const std::int64_t sum =
        static_cast<std::int64_t>(bus[frame * 2u]) + bus[frame * 2u + 1u];
    return static_cast<std::int32_t>(sum / 2);
}

std::int32_t GlobalEffectsProcessor::triangleQ15(std::uint32_t phase_q32) {
    constexpr std::uint32_t kQuarterMask = (std::uint32_t{1} << 30u) - 1u;
    const std::uint32_t quadrant = phase_q32 >> 30u;
    const std::uint32_t fraction = phase_q32 & kQuarterMask;
    const std::int32_t ramp = static_cast<std::int32_t>(
        (static_cast<std::uint64_t>(fraction) * kQ15Unity +
         (std::uint64_t{1} << 29u)) >> 30u);
    switch (quadrant) {
        case 0:
            return ramp;
        case 1:
            return kQ15Unity - ramp;
        case 2:
            return -ramp;
        default:
            return -kQ15Unity + ramp;
    }
}

std::int32_t GlobalEffectsProcessor::processComb(
    std::int32_t input,
    std::int16_t* buffer,
    std::size_t size,
    std::size_t* index,
    std::int32_t feedback_q15) {
    const std::int32_t delayed = buffer[*index];
    const std::int32_t feedback = scaleQ15(delayed, feedback_q15);
    buffer[*index] = clampI16(static_cast<std::int32_t>(std::clamp<std::int64_t>(
        static_cast<std::int64_t>(input) + feedback,
        std::numeric_limits<std::int32_t>::min(),
        std::numeric_limits<std::int32_t>::max())));
    advanceRingIndex(index, size);
    return delayed;
}

std::int32_t GlobalEffectsProcessor::processAllpass(
    std::int32_t input,
    std::int16_t* buffer,
    std::size_t size,
    std::size_t* index) {
    const std::int32_t delayed = buffer[*index];
    // g=0.5 all-pass: y = delayed - g*x, state = x + g*y.
    const std::int32_t output = delayed - input / 2;
    const std::int64_t next = static_cast<std::int64_t>(input) + output / 2;
    buffer[*index] = clampI16(static_cast<std::int32_t>(std::clamp<std::int64_t>(
        next,
        std::numeric_limits<std::int32_t>::min(),
        std::numeric_limits<std::int32_t>::max())));
    advanceRingIndex(index, size);
    return output;
}

std::int32_t GlobalEffectsProcessor::chorusTap(std::size_t delay_frames) const {
    const std::size_t size = chorus_delay_.size();
    const std::size_t delay = std::clamp<std::size_t>(delay_frames, 1u, size - 1u);
    const std::size_t read = chorus_write_ >= delay
        ? chorus_write_ - delay
        : chorus_write_ + size - delay;
    return chorus_delay_[read];
}

void GlobalEffectsProcessor::processAdd(
    const std::int32_t* chorus_bus,
    const std::int32_t* reverb_bus,
    std::int32_t* interleaved_stereo,
    std::size_t frame_count) {
    if (interleaved_stereo == nullptr || frame_count == 0) {
        return;
    }

    for (std::size_t frame = 0; frame < frame_count; ++frame) {
        const std::int32_t chorus_input = averageStereo(chorus_bus, frame);
        chorus_delay_[chorus_write_] = clampI16(chorus_input);
        const std::int32_t lfo_q15 = triangleQ15(chorus_phase_q32_);
        // lfo_q15 is +/-32767 and depth is 96, so this exact product remains
        // well inside int32 while preserving C++ signed division semantics.
        const std::int32_t modulation =
            (lfo_q15 * static_cast<std::int32_t>(kChorusDepthFrames)) / kQ15Unity;
        const std::size_t left_delay = static_cast<std::size_t>(
            static_cast<std::int32_t>(kChorusBaseDelayFrames) + modulation);
        const std::size_t right_delay = static_cast<std::size_t>(
            static_cast<std::int32_t>(kChorusBaseDelayFrames) - modulation);
        const std::int32_t chorus_left = scaleQ15(chorusTap(left_delay), kChorusWetGainQ15);
        const std::int32_t chorus_right = scaleQ15(chorusTap(right_delay), kChorusWetGainQ15);
        advanceRingIndex(&chorus_write_, chorus_delay_.size());
        chorus_phase_q32_ += kChorusPhaseStepQ32;

        const std::int32_t reverb_input = averageStereo(reverb_bus, frame);
        const std::int32_t comb_a = processComb(
            reverb_input,
            reverb_comb_a_.data(),
            reverb_comb_a_.size(),
            &reverb_comb_a_index_,
            kReverbFeedbackAQ15);
        const std::int32_t comb_b = processComb(
            reverb_input,
            reverb_comb_b_.data(),
            reverb_comb_b_.size(),
            &reverb_comb_b_index_,
            kReverbFeedbackBQ15);
        const std::int32_t comb_c = processComb(
            reverb_input,
            reverb_comb_c_.data(),
            reverb_comb_c_.size(),
            &reverb_comb_c_index_,
            kReverbFeedbackCQ15);

        const std::int32_t reverb_left_seed = (comb_a + comb_b + comb_c) / 3;
        const std::int32_t reverb_right_seed = (comb_a - comb_b + comb_c) / 3;
        const std::int32_t reverb_left = scaleQ15(
            processAllpass(
                reverb_left_seed,
                reverb_allpass_l_.data(),
                reverb_allpass_l_.size(),
                &reverb_allpass_l_index_),
            kReverbWetGainQ15);
        const std::int32_t reverb_right = scaleQ15(
            processAllpass(
                reverb_right_seed,
                reverb_allpass_r_.data(),
                reverb_allpass_r_.size(),
                &reverb_allpass_r_index_),
            kReverbWetGainQ15);

        interleaved_stereo[frame * 2u] += chorus_left + reverb_left;
        interleaved_stereo[frame * 2u + 1u] += chorus_right + reverb_right;
    }
}

}  // namespace gm_synth::voice
