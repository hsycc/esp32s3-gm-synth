#include "VoiceRenderer.h"
#include "RealtimeModulationMath.h"

#include "util/SaturatingCounter.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>

namespace gm_synth::voice {
namespace {

constexpr std::size_t kEffectsScratchFrames = 64;
constexpr std::size_t kChannels = 2;
// Keep this synchronized with VoiceRenderer.cpp. Complex continuous Voices use
// the same control cadence as the reference renderer; only invariant loop/sample
// metadata is hoisted out of the per-frame path.
constexpr std::uint8_t kRealtimeModulationControlFrames = 16;

std::uint32_t magnitudeI32(std::int32_t value) {
    const std::uint32_t bits = static_cast<std::uint32_t>(value);
    return value < 0 ? 0u - bits : bits;
}

std::int32_t scaleSend(std::int32_t sample, std::int32_t units) {
    const std::int32_t clamped = std::clamp<std::int32_t>(units, 0, kMaxEffectsSendUnits);
    if (sample == 0 || clamped == 0) {
        return 0;
    }
    if (clamped == kMaxEffectsSendUnits) {
        return sample;
    }
    if (clamped == kMaxEffectsSendUnits / 2) {
        // Exact specialization of round(sample * 500 / 1000) with the same
        // symmetric half-away-from-zero behavior as the generic formula.
        // C++ signed / and % truncate toward zero, so q+r is exact here.
        return sample / 2 + sample % 2;
    }

    // Exact decomposition of round(abs(sample) * units / 1000). Splitting the
    // sample magnitude first keeps every divide and product 32-bit, avoiding
    // Xtensa __divdi3 while preserving the previous half-away-from-zero result.
    const std::uint32_t magnitude = magnitudeI32(sample);
    const std::uint32_t whole = magnitude / static_cast<std::uint32_t>(kMaxEffectsSendUnits);
    const std::uint32_t remainder =
        magnitude - whole * static_cast<std::uint32_t>(kMaxEffectsSendUnits);
    const std::uint32_t scaled =
        whole * static_cast<std::uint32_t>(clamped) +
        (remainder * static_cast<std::uint32_t>(clamped) +
         static_cast<std::uint32_t>(kMaxEffectsSendUnits / 2)) /
            static_cast<std::uint32_t>(kMaxEffectsSendUnits);
    return sample < 0 ? -static_cast<std::int32_t>(scaled)
                      : static_cast<std::int32_t>(scaled);
}

std::int32_t scaleSampleQ15Fast(std::int32_t sample, std::int32_t gain_q15) {
    if (gain_q15 >= kQ15Unity) {
        return sample;
    }
    if (gain_q15 <= 0) {
        return 0;
    }

    // The simple fast path interpolates int16 PCM, so sample remains in
    // [-32768, 32767]. gain_q15 is clamped to [0, 32767]. Their product is
    // therefore safely inside int32; using a 64-bit multiply here only adds
    // cost on Xtensa and does not provide extra numerical precision.
    const std::int32_t product = sample * gain_q15;
    if (product >= 0) {
        return (product + (std::int32_t{1} << 14)) >> 15;
    }
    return -((-product + (std::int32_t{1} << 14)) >> 15);
}

std::int32_t scaleSampleQ15Wide(std::int32_t sample, std::int32_t gain_q15) {
    if (gain_q15 >= kQ15Unity) {
        return sample;
    }
    if (gain_q15 <= 0) {
        return 0;
    }

    // Filtered samples are not constrained to int16 range, so the complex
    // realtime path deliberately uses the same wide arithmetic and rounding as
    // VoiceRenderer.cpp::scaleSampleQ15(). This keeps it bit-equivalent to the
    // reference renderer even for resonant-filter overshoot.
    const std::int64_t product = static_cast<std::int64_t>(sample) * gain_q15;
    if (product >= 0) {
        return static_cast<std::int32_t>((product + (std::int64_t{1} << 14)) >> 15);
    }
    return -static_cast<std::int32_t>(((-product) + (std::int64_t{1} << 14)) >> 15);
}

std::uint64_t wrapContinuousPhase(
    std::uint64_t phase,
    std::uint64_t loop_start_q32,
    std::uint64_t loop_end_q32,
    std::uint32_t loop_length_frames) {
    if (phase < loop_end_q32 || loop_length_frames == 0u) {
        return phase;
    }

    const std::uint64_t loop_length_q32 =
        static_cast<std::uint64_t>(loop_length_frames) << 32u;
    const std::uint64_t overshoot = phase - loop_end_q32;
    if (overshoot < loop_length_q32) {
        return loop_start_q32 + overshoot;
    }

    // The loop modulus is always an integer-frame span (N << 32). Decompose
    // the Q32 delta into whole frames + fractional bits, perform only a 32-bit
    // frame modulo, then reassemble. This is exactly delta % (N << 32) without
    // pulling __umoddi3 into the realtime continuous-loop object.
    const std::uint64_t delta = phase - loop_start_q32;
    const std::uint32_t whole_frames = static_cast<std::uint32_t>(delta >> 32u);
    const std::uint32_t wrapped_frames = whole_frames % loop_length_frames;
    const std::uint32_t fraction = static_cast<std::uint32_t>(delta);
    return loop_start_q32 +
        (static_cast<std::uint64_t>(wrapped_frames) << 32u) + fraction;
}

}  // namespace

// BEGIN V1_REALTIME_FAST_PATH
#if GM_SYNTH_EXPERIMENTAL_VOICE_LOOP
namespace {

// Exact floor(amplitude * position / total), then one-unit position steps.
// The initial division is once per envelope segment, never once per sample.
// A rare long-envelope numerator retains the wide divide at segment setup.
// Remainder updates avoid overflowing uint32 even when total is UINT32_MAX.
struct StaticEnvelopeRamp {
    std::uint32_t value = 0, remainder = 0, whole_step = 0, remainder_step = 0;
    std::uint32_t total = 1;
    void start(std::uint32_t amplitude, std::uint32_t position, std::uint32_t divisor) {
        total = divisor;
        const std::uint64_t numerator = std::uint64_t{amplitude} * position;
        value = static_cast<std::uint32_t>(numerator <= UINT32_MAX
            ? static_cast<std::uint32_t>(numerator) / total : numerator / total);
        remainder = static_cast<std::uint32_t>(numerator - std::uint64_t{value} * total);
        whole_step = amplitude / total;
        remainder_step = amplitude - whole_step * total;
    }
    void rise() {
        value += whole_step;
        if (remainder >= total - remainder_step) {
            ++value;
            remainder -= total - remainder_step;
        } else {
            remainder += remainder_step;
        }
    }
    void fall() {
        value -= whole_step;
        if (remainder < remainder_step) {
            --value;
            remainder = total - (remainder_step - remainder);
        } else {
            remainder -= remainder_step;
        }
    }
};

std::int32_t staticFilterRoundQ24(std::int64_t value) {
    constexpr std::int64_t round = std::int64_t{1} << (kFilterCoefficientFractionBits - 1u);
    const std::int64_t rounded = value >= 0
        ? (value + round) >> kFilterCoefficientFractionBits
        : -(((-value) + round) >> kFilterCoefficientFractionBits);
    return static_cast<std::int32_t>(std::clamp<std::int64_t>(rounded,
        std::numeric_limits<std::int32_t>::min(), std::numeric_limits<std::int32_t>::max()));
}

}  // namespace

void Voice::advanceStaticVibrato(std::size_t frames) {
    // A zero-depth oscillator is inaudible now, but its phase and control tick
    // must still match the reference when a later CC1/pressure event adds depth.
    if (!params_.vibrato_lfo.enabled || frames == 0) return;
    if (frames <= vibrato_delay_remaining_) {
        vibrato_delay_remaining_ -= static_cast<std::uint32_t>(frames);
        if (vibrato_delay_remaining_ == 0) vibrato_control_frames_remaining_ = 0;
        return;
    }
    if (vibrato_delay_remaining_) {
        frames -= vibrato_delay_remaining_;
        vibrato_delay_remaining_ = 0;
        vibrato_control_frames_remaining_ = 0;
    }
    vibrato_phase_q32_ += params_.vibrato_lfo.phase_step_q32 * static_cast<std::uint32_t>(frames);
    if (frames < vibrato_control_frames_remaining_) {
        vibrato_control_frames_remaining_ -= static_cast<std::uint8_t>(frames);
    } else {
        const auto after_tick = (frames - vibrato_control_frames_remaining_) %
            kRealtimeModulationControlFrames;
        vibrato_control_frames_remaining_ = static_cast<std::uint8_t>(
            (kRealtimeModulationControlFrames - after_tick) % kRealtimeModulationControlFrames);
    }
}

VoiceRenderResult Voice::renderAddStaticRealtime(
    std::int32_t* interleaved_stereo, std::size_t frame_count) {
    const auto region_start = playbackStartFrame();
    const auto end_frame = playbackEndFrame();
    const auto loop_start = playbackLoopStartFrame();
    const auto loop_end = playbackLoopEndFrame();
    if (!(region_start <= loop_start && loop_start < loop_end && loop_end <= end_frame))
        return renderAdd(interleaved_stereo, frame_count);
    const auto* const pcm = ref_.record->pcm;
    const auto loop_start_q32 = std::uint64_t{loop_start} << 32;
    const auto loop_end_q32 = std::uint64_t{loop_end} << 32;
    const auto loop_length = loop_end - loop_start;
    const auto step = current_phase_increment_q32_;
    const auto filter = current_lowpass_;
    auto z1 = lowpass_z1_q24_, z2 = lowpass_z2_q24_;
    auto phase = phase_q32_;
    VoiceRenderResult result{};
    if (loop_execution_enabled_ && !loop_cursor_.setStep(step)) {
        finish(); result.finished = true; return result;
    }

    while (result.frames_rendered < frame_count && active_) {
        const auto stage = envelope_stage_;
        std::uint32_t total = 0;
        std::int32_t constant_envelope = 0;
        StaticEnvelopeRamp ramp;
        switch (stage) {
            case EnvelopeStage::Delay: total = params_.envelope.delay_frames; break;
            case EnvelopeStage::Attack: total = params_.envelope.attack_frames; break;
            case EnvelopeStage::Hold:
                total = params_.envelope.hold_frames; constant_envelope = kQ15Unity; break;
            case EnvelopeStage::Decay: total = params_.envelope.decay_frames; break;
            case EnvelopeStage::Sustain: constant_envelope = params_.envelope.sustain_q15; break;
            case EnvelopeStage::Release: total = active_release_frames_; break;
            case EnvelopeStage::Done: finish(); break;
        }
        if (!active_) break;
        // Normally unreachable: all zero-duration transitions skip these stages.
        // Preserve the reference's defensive behavior rather than dividing by 0.
        if (stage != EnvelopeStage::Sustain && (total == 0 || envelope_stage_frame_ >= total)) {
            phase_q32_ = phase; lowpass_z1_q24_ = z1; lowpass_z2_q24_ = z2;
            advanceStaticVibrato(result.frames_rendered);
            const auto rest = renderAdd(interleaved_stereo + result.frames_rendered * kChannels,
                                        frame_count - result.frames_rendered);
            result.frames_rendered += rest.frames_rendered;
            result.finished = rest.finished;
            return result;
        }
        const auto chunk = stage == EnvelopeStage::Sustain
            ? frame_count - result.frames_rendered
            : std::min<std::size_t>(frame_count - result.frames_rendered, total - envelope_stage_frame_);
        const bool attack = stage == EnvelopeStage::Attack;
        const bool decay = stage == EnvelopeStage::Decay;
        const bool release = stage == EnvelopeStage::Release;
        const bool varying = attack || decay || release;
        if (attack) ramp.start(kQ15Unity, envelope_stage_frame_ + 1u, total);
        if (decay) ramp.start(kQ15Unity - params_.envelope.sustain_q15, envelope_stage_frame_ + 1u, total);
        if (release) ramp.start(static_cast<std::uint32_t>(release_start_level_q15_),
                                total - envelope_stage_frame_, total);
        const auto constant_left = multiplyQ15(current_gain_left_q15_, constant_envelope);
        const auto constant_right = multiplyQ15(current_gain_right_q15_, constant_envelope);
        const bool shared_gain = current_gain_left_q15_ == current_gain_right_q15_;
        std::size_t rendered = 0;
        for (; rendered < chunk; ++rendered) {
            if ((phase >> 32) >= end_frame) { finish(); break; }
            const auto envelope = varying
                ? static_cast<std::int32_t>(decay ? kQ15Unity - ramp.value : ramp.value)
                : constant_envelope;
            // The reference suppresses a terminal zero-valued release frame.
            if (release && envelope_stage_frame_ + rendered + 1u == total && envelope <= 0) {
                finish(); break;
            }
            std::int32_t source = 0;
            if (loop_execution_enabled_) {
                std::int16_t raw = 0;
                if (!loop_cursor_.next(raw)) { finish(); break; }
                source = raw;
            } else {
                const auto index = static_cast<std::uint32_t>(phase >> 32);
                auto next_index = index + 1u;
                if (index >= loop_start && next_index >= loop_end) next_index = loop_start;
                else if (next_index >= end_frame) next_index = end_frame - 1u;
                const std::int32_t a = pcm[index], b = pcm[next_index];
                source = static_cast<std::int32_t>(std::int64_t{a} +
                    ((std::int64_t{b - a} * static_cast<std::uint32_t>(phase)) >> 32));
            }
            auto mono = source;
            if (filter.enabled) {
                const std::int64_t input_a0 = std::int64_t{source} * filter.a0_q24;
                mono = staticFilterRoundQ24(input_a0 + z1);
                z1 = std::int64_t{source} * filter.a1_q24 + z2 - std::int64_t{mono} * filter.b1_q24;
                z2 = input_a0 - std::int64_t{mono} * filter.b2_q24;
            }
            const auto left_gain = varying ? multiplyQ15(current_gain_left_q15_, envelope) : constant_left;
            const auto left = scaleSampleQ15Wide(mono, left_gain);
            const auto at = (result.frames_rendered + rendered) * kChannels;
            interleaved_stereo[at] += left;
            interleaved_stereo[at + 1u] += shared_gain ? left : scaleSampleQ15Wide(mono,
                varying ? multiplyQ15(current_gain_right_q15_, envelope) : constant_right);
            envelope_level_q15_ = envelope;
            if (loop_execution_enabled_) {
                phase = loop_cursor_.phaseQ32();
                if (!loop_cursor_.active()) { ++rendered; finish(); break; }
            } else {
                phase += step;
                phase = wrapContinuousPhase(phase, loop_start_q32, loop_end_q32, loop_length);
            }
            if (varying && rendered + 1u < chunk) {
                if (release) ramp.fall(); else ramp.rise();
            }
        }
        result.frames_rendered += rendered;
        if (!active_) break;
        if (stage == EnvelopeStage::Sustain) continue;
        envelope_stage_frame_ += static_cast<std::uint32_t>(rendered);
        if (envelope_stage_frame_ < total) continue;
        envelope_stage_frame_ = 0;
        switch (stage) {
            case EnvelopeStage::Delay:
                envelope_stage_ = params_.envelope.attack_frames ? EnvelopeStage::Attack :
                    params_.envelope.hold_frames ? EnvelopeStage::Hold :
                    params_.envelope.decay_frames ? EnvelopeStage::Decay : EnvelopeStage::Sustain;
                break;
            case EnvelopeStage::Attack:
                envelope_level_q15_ = kQ15Unity;
                envelope_stage_ = params_.envelope.hold_frames ? EnvelopeStage::Hold :
                    params_.envelope.decay_frames ? EnvelopeStage::Decay : EnvelopeStage::Sustain;
                break;
            case EnvelopeStage::Hold:
                envelope_stage_ = params_.envelope.decay_frames ? EnvelopeStage::Decay : EnvelopeStage::Sustain;
                break;
            case EnvelopeStage::Decay:
                envelope_level_q15_ = params_.envelope.sustain_q15;
                envelope_stage_ = EnvelopeStage::Sustain;
                break;
            case EnvelopeStage::Release: finish(); break;
            default: break;
        }
    }
    if (active_) {
        phase_q32_ = phase; lowpass_z1_q24_ = z1; lowpass_z2_q24_ = z2;
        advanceStaticVibrato(result.frames_rendered);
    }
    result.finished = !active_;
    return result;
}
#endif
// END V1_REALTIME_FAST_PATH
VoiceRenderResult Voice::renderAddRealtime(
    std::int32_t* interleaved_stereo,
    std::size_t frame_count) {
    if (!active_ || interleaved_stereo == nullptr || frame_count == 0) {
        VoiceRenderResult result{};
        result.finished = !active_;
        return result;
    }

    // BEGIN V1_REALTIME_FAST_PATH
#if GM_SYNTH_EXPERIMENTAL_VOICE_LOOP
    // Static filter/gain/pitch is common in real SF2 playback even when a
    // zero-depth vibrato oscillator is armed for later controller changes.
    if ((loop_execution_enabled_ || current_lowpass_.enabled || params_.vibrato_lfo.enabled) &&
        params_.loop_mode == LoopMode::Continuous && ref_.valid() && ref_.record->pcm &&
        !params_.mod_lfo.enabled && !params_.mod_envelope.enabled &&
        !params_.dynamic_lowpass_table.enabled &&
        (!params_.vibrato_lfo.enabled || params_.vibrato_lfo.pitch_depth_cents == 0))
        return renderAddStaticRealtime(interleaved_stereo, frame_count);
#endif
    // END V1_REALTIME_FAST_PATH
#if GM_SYNTH_EXPERIMENTAL_VOICE_LOOP
    // Correctness-first opt-in path. Existing fast paths stay untouched for
    // Direct voices; crossfade voices reuse reference DSP and event ordering.
    // This is NOT a qualified optimized crossfade hot path on ESP32-S3.
    if (loop_execution_enabled_) return renderAdd(interleaved_stereo, frame_count);
#endif
    // Continuous SF2 loops are the common resident-sample realtime case. Hoist
    // their immutable sample/region/loop metadata once per render segment even
    // when filter/LFO/mod-envelope DSP is enabled. Non-continuous playback keeps
    // using renderAdd() as the canonical reference path.
    if (params_.loop_mode != LoopMode::Continuous || !ref_.valid() ||
        ref_.record == nullptr || ref_.record->pcm == nullptr) {
        return renderAdd(interleaved_stereo, frame_count);
    }

    const std::uint32_t region_start = playbackStartFrame();
    const std::uint32_t end_frame = playbackEndFrame();
    const std::uint32_t loop_start = playbackLoopStartFrame();
    const std::uint32_t loop_end = playbackLoopEndFrame();
    if (!(region_start <= loop_start && loop_start < loop_end && loop_end <= end_frame)) {
        return renderAdd(interleaved_stereo, frame_count);
    }

    const std::int16_t* const pcm = ref_.record->pcm;
    const std::uint64_t loop_start_q32 = static_cast<std::uint64_t>(loop_start) << 32;
    const std::uint64_t loop_end_q32 = static_cast<std::uint64_t>(loop_end) << 32;
    const std::uint32_t loop_length_frames = loop_end - loop_start;
    if (loop_length_frames == 0u) {
        return renderAdd(interleaved_stereo, frame_count);
    }

    std::uint64_t phase = phase_q32_;
    std::int32_t* output = interleaved_stereo;
    VoiceRenderResult result{};

    // Preserve the original lowest-cost specialization for dry/unmodulated
    // continuous Voices. Complex Voices below reuse the same hoisted playback
    // metadata but retain reference modulation/filter arithmetic and ordering.
    const bool simple_continuous =
        !params_.vibrato_lfo.enabled &&
        !params_.mod_lfo.enabled &&
        !params_.mod_envelope.enabled &&
        !params_.dynamic_lowpass_table.enabled &&
        !current_lowpass_.enabled;
    if (simple_continuous) {
        const std::uint64_t phase_increment = current_phase_increment_q32_;
        const bool steady_sustain =
            envelope_stage_ == EnvelopeStage::Sustain && !key_released_;
        if (steady_sustain) {
            const std::int32_t envelope = params_.envelope.sustain_q15;
            envelope_level_q15_ = envelope;
            const std::int32_t left_gain = multiplyQ15(current_gain_left_q15_, envelope);
            const std::int32_t right_gain = multiplyQ15(current_gain_right_q15_, envelope);
            const bool shared_gain = left_gain == right_gain;

            for (std::size_t frame = 0; frame < frame_count; ++frame) {
                const std::uint32_t index = static_cast<std::uint32_t>(phase >> 32);
                if (index >= end_frame) {
                    phase_q32_ = phase;
                    finish();
                    break;
                }

                std::uint32_t next_index = index + 1u;
                if (index >= loop_start && next_index >= loop_end) {
                    next_index = loop_start;
                } else if (next_index >= end_frame) {
                    next_index = end_frame - 1u;
                }

                const std::int32_t a = pcm[index];
                const std::int32_t b = pcm[next_index];
                const std::uint32_t fraction = static_cast<std::uint32_t>(phase & 0xffffffffu);
                const std::int64_t delta = static_cast<std::int64_t>(b) - a;
                const std::int32_t mono = static_cast<std::int32_t>(
                    static_cast<std::int64_t>(a) +
                    ((delta * static_cast<std::int64_t>(fraction)) >> 32));

                if (shared_gain) {
                    const std::int32_t scaled = scaleSampleQ15Fast(mono, left_gain);
                    output[0] += scaled;
                    output[1] += scaled;
                } else {
                    output[0] += scaleSampleQ15Fast(mono, left_gain);
                    output[1] += scaleSampleQ15Fast(mono, right_gain);
                }
                output += kChannels;
                ++result.frames_rendered;

                phase += phase_increment;
                phase = wrapContinuousPhase(
                    phase,
                    loop_start_q32,
                    loop_end_q32,
                    loop_length_frames);
            }
        } else {
            // Release is the dominant non-sustain physical stress state. The
            // canonical envelope computes release_start * remaining / total on
            // every sample. For the overwhelmingly common range where that
            // numerator fits uint32, compute one exact divmod at segment start
            // and advance quotient/remainder with a Bresenham-style recurrence.
            // This preserves every Q15 envelope value and finish boundary while
            // removing 63 of 64 divisions per Voice from a normal audio block.
            const bool release_stage =
                envelope_stage_ == EnvelopeStage::Release &&
                active_release_frames_ != 0u &&
                envelope_stage_frame_ < active_release_frames_ &&
                release_start_level_q15_ > 0;
            const std::uint32_t release_start = release_stage
                ? static_cast<std::uint32_t>(clampQ15(release_start_level_q15_))
                : 0u;
            const std::uint32_t release_remaining = release_stage
                ? active_release_frames_ - envelope_stage_frame_
                : 0u;
            const std::uint64_t release_numerator_wide =
                static_cast<std::uint64_t>(release_start) * release_remaining;
            const bool release_fast_path =
                release_stage &&
                release_numerator_wide <= std::numeric_limits<std::uint32_t>::max();

            if (release_fast_path) {
                const std::uint32_t total = active_release_frames_;
                const std::uint32_t numerator =
                    static_cast<std::uint32_t>(release_numerator_wide);
                std::int32_t envelope = static_cast<std::int32_t>(numerator / total);
                std::uint32_t remainder =
                    numerator - static_cast<std::uint32_t>(envelope) * total;
                const std::uint32_t step_whole = release_start / total;
                const std::uint32_t step_remainder =
                    release_start - step_whole * total;
                const bool shared_base_gain =
                    current_gain_left_q15_ == current_gain_right_q15_;

                for (std::size_t frame = 0; frame < frame_count && active_; ++frame) {
                    const std::uint32_t index = static_cast<std::uint32_t>(phase >> 32);
                    if (index >= end_frame) {
                        phase_q32_ = phase;
                        finish();
                        break;
                    }

                    envelope_level_q15_ = envelope;
                    ++envelope_stage_frame_;
                    if (envelope_stage_frame_ >= total) {
                        envelope_stage_ = EnvelopeStage::Done;
                    }
                    if (envelope_stage_ == EnvelopeStage::Done && envelope <= 0) {
                        phase_q32_ = phase;
                        finish();
                        break;
                    }

                    std::uint32_t next_index = index + 1u;
                    if (index >= loop_start && next_index >= loop_end) {
                        next_index = loop_start;
                    } else if (next_index >= end_frame) {
                        next_index = end_frame - 1u;
                    }

                    const std::int32_t a = pcm[index];
                    const std::int32_t b = pcm[next_index];
                    const std::uint32_t fraction =
                        static_cast<std::uint32_t>(phase & 0xffffffffu);
                    const std::int64_t delta = static_cast<std::int64_t>(b) - a;
                    const std::int32_t mono = static_cast<std::int32_t>(
                        static_cast<std::int64_t>(a) +
                        ((delta * static_cast<std::int64_t>(fraction)) >> 32));

                    const std::int32_t left_gain =
                        multiplyQ15(current_gain_left_q15_, envelope);
                    if (shared_base_gain) {
                        const std::int32_t scaled = scaleSampleQ15Fast(mono, left_gain);
                        output[0] += scaled;
                        output[1] += scaled;
                    } else {
                        const std::int32_t right_gain =
                            multiplyQ15(current_gain_right_q15_, envelope);
                        output[0] += scaleSampleQ15Fast(mono, left_gain);
                        output[1] += scaleSampleQ15Fast(mono, right_gain);
                    }
                    output += kChannels;
                    ++result.frames_rendered;

                    const bool release_finishes_after_frame =
                        envelope_stage_ == EnvelopeStage::Done;
                    phase += phase_increment;
                    phase = wrapContinuousPhase(
                        phase,
                        loop_start_q32,
                        loop_end_q32,
                        loop_length_frames);
                    if (release_finishes_after_frame && active_) {
                        phase_q32_ = phase;
                        finish();
                    }
                    if (!active_) {
                        break;
                    }

                    if (remainder >= step_remainder) {
                        envelope -= static_cast<std::int32_t>(step_whole);
                        remainder -= step_remainder;
                    } else {
                        envelope -= static_cast<std::int32_t>(step_whole + 1u);
                        remainder = remainder + total - step_remainder;
                    }
                }
            } else {
                // Attack/hold/decay and extremely long releases keep using the
                // canonical helper. That makes the optimization correctness-
                // preserving even when release_start * remaining exceeds u32.
                for (std::size_t frame = 0; frame < frame_count && active_; ++frame) {
                    const std::uint32_t index = static_cast<std::uint32_t>(phase >> 32);
                    if (index >= end_frame) {
                        phase_q32_ = phase;
                        finish();
                        break;
                    }

                    const std::int32_t envelope = nextEnvelopeLevel();
                    if (envelope_stage_ == EnvelopeStage::Done && envelope <= 0) {
                        phase_q32_ = phase;
                        finish();
                        break;
                    }

                    std::uint32_t next_index = index + 1u;
                    if (index >= loop_start && next_index >= loop_end) {
                        next_index = loop_start;
                    } else if (next_index >= end_frame) {
                        next_index = end_frame - 1u;
                    }

                    const std::int32_t a = pcm[index];
                    const std::int32_t b = pcm[next_index];
                    const std::uint32_t fraction =
                        static_cast<std::uint32_t>(phase & 0xffffffffu);
                    const std::int64_t delta = static_cast<std::int64_t>(b) - a;
                    const std::int32_t mono = static_cast<std::int32_t>(
                        static_cast<std::int64_t>(a) +
                        ((delta * static_cast<std::int64_t>(fraction)) >> 32));

                    const std::int32_t left_gain =
                        multiplyQ15(current_gain_left_q15_, envelope);
                    if (current_gain_left_q15_ == current_gain_right_q15_) {
                        const std::int32_t scaled = scaleSampleQ15Fast(mono, left_gain);
                        output[0] += scaled;
                        output[1] += scaled;
                    } else {
                        const std::int32_t right_gain =
                            multiplyQ15(current_gain_right_q15_, envelope);
                        output[0] += scaleSampleQ15Fast(mono, left_gain);
                        output[1] += scaleSampleQ15Fast(mono, right_gain);
                    }
                    output += kChannels;
                    ++result.frames_rendered;

                    const bool release_finishes_after_frame =
                        envelope_stage_ == EnvelopeStage::Done;
                    phase += phase_increment;
                    phase = wrapContinuousPhase(
                        phase,
                        loop_start_q32,
                        loop_end_q32,
                        loop_length_frames);
                    if (release_finishes_after_frame && active_) {
                        phase_q32_ = phase;
                        finish();
                    }
                }
            }
        }
    } else {
        // Complex continuous path: execute exactly the same state transitions as
        // renderAdd(), but avoid rebuilding region/loop state and re-validating
        // the loop on every sample. Modulation refresh uses an algebraically
        // exact 32-bit-division implementation so ESP32-S3 stays off __divdi3.
        for (std::size_t frame = 0; frame < frame_count && active_; ++frame) {
            const std::uint32_t index = static_cast<std::uint32_t>(phase >> 32);
            if (index >= end_frame) {
                phase_q32_ = phase;
                finish();
                break;
            }

            const bool vibrato_due = params_.vibrato_lfo.enabled &&
                vibrato_delay_remaining_ == 0 && vibrato_control_frames_remaining_ == 0;
            const bool mod_lfo_due = params_.mod_lfo.enabled &&
                mod_lfo_delay_remaining_ == 0 && mod_lfo_control_frames_remaining_ == 0;
            const bool mod_envelope_due = params_.mod_envelope.enabled &&
                mod_envelope_control_frames_remaining_ == 0;

            (void)nextModEnvelopeLevel();
            if (vibrato_due || mod_lfo_due || mod_envelope_due) {
                const realtime_modulation::State modulation =
                    realtime_modulation::compute(
                        params_,
                        vibrato_phase_q32_,
                        vibrato_delay_remaining_,
                        mod_lfo_phase_q32_,
                        mod_lfo_delay_remaining_,
                        modEnvelopeDspLevelQ15());
                current_phase_increment_q32_ = modulation.phase_increment_q32;
                current_gain_left_q15_ = modulation.gain_left_q15;
                current_gain_right_q15_ = modulation.gain_right_q15;
                current_lowpass_ = modulation.lowpass;
                if (params_.vibrato_lfo.enabled && vibrato_delay_remaining_ == 0) {
                    vibrato_control_frames_remaining_ = kRealtimeModulationControlFrames;
                }
                if (params_.mod_lfo.enabled && mod_lfo_delay_remaining_ == 0) {
                    mod_lfo_control_frames_remaining_ = kRealtimeModulationControlFrames;
                }
                if (params_.mod_envelope.enabled) {
                    mod_envelope_control_frames_remaining_ = kRealtimeModulationControlFrames;
                }
            }

            const std::int32_t envelope = nextEnvelopeLevel();
            if (envelope_stage_ == EnvelopeStage::Done && envelope <= 0) {
                phase_q32_ = phase;
                finish();
                break;
            }

            std::uint32_t next_index = index + 1u;
            if (index >= loop_start && next_index >= loop_end) {
                next_index = loop_start;
            } else if (next_index >= end_frame) {
                next_index = end_frame - 1u;
            }

            const std::int32_t a = pcm[index];
            const std::int32_t b = pcm[next_index];
            const std::uint32_t fraction = static_cast<std::uint32_t>(phase & 0xffffffffu);
            const std::int64_t delta = static_cast<std::int64_t>(b) - a;
            const std::int32_t interpolated = static_cast<std::int32_t>(
                static_cast<std::int64_t>(a) +
                ((delta * static_cast<std::int64_t>(fraction)) >> 32));
            const std::int32_t mono = applyLowpass(interpolated);
            const std::int32_t left_gain = multiplyQ15(current_gain_left_q15_, envelope);
            if (current_gain_left_q15_ == current_gain_right_q15_) {
                const std::int32_t scaled = scaleSampleQ15Wide(mono, left_gain);
                output[0] += scaled;
                output[1] += scaled;
            } else {
                const std::int32_t right_gain = multiplyQ15(current_gain_right_q15_, envelope);
                output[0] += scaleSampleQ15Wide(mono, left_gain);
                output[1] += scaleSampleQ15Wide(mono, right_gain);
            }
            output += kChannels;
            ++result.frames_rendered;

            const bool release_finishes_after_frame =
                envelope_stage_ == EnvelopeStage::Done;
            phase += current_phase_increment_q32_;
            phase = wrapContinuousPhase(
                phase,
                loop_start_q32,
                loop_end_q32,
                loop_length_frames);
            advanceLfos();
            if (release_finishes_after_frame && active_) {
                phase_q32_ = phase;
                finish();
            }
        }
    }

    if (active_) {
        phase_q32_ = phase;
    }
    result.finished = !active_;
    return result;
}

void VoicePool::renderAddWithEffects(
    std::int32_t* interleaved_stereo,
    std::int32_t* reverb_send_stereo,
    std::int32_t* chorus_send_stereo,
    std::size_t frame_count) {
    if (interleaved_stereo == nullptr || frame_count == 0) {
        return;
    }

    // The scratch array starts at zero once. Each send-distribution pass clears
    // the slots as it consumes them, so the next Voice sees a zeroed scratch
    // buffer without a separate fill_n memory pass.
    std::array<std::int32_t, kEffectsScratchFrames * kChannels> scratch{};
    std::size_t frame_offset = 0;
    while (frame_offset < frame_count) {
        const std::size_t chunk_frames = std::min<std::size_t>(
            kEffectsScratchFrames,
            frame_count - frame_offset);
        const std::size_t chunk_samples = chunk_frames * kChannels;
        const std::size_t sample_offset = frame_offset * kChannels;

        for (Voice& voice : voices_) {
            if (!voice.active()) {
                continue;
            }

            const std::int32_t reverb_units = voice.reverbSendUnits();
            const std::int32_t chorus_units = voice.chorusSendUnits();
            const bool reverb_active = reverb_send_stereo != nullptr && reverb_units != 0;
            const bool chorus_active = chorus_send_stereo != nullptr && chorus_units != 0;
            const bool needs_send_scratch = reverb_active || chorus_active;
            const bool shared_send_scale =
                reverb_active && chorus_active && reverb_units == chorus_units;

            VoiceRenderResult result{};
            if (!needs_send_scratch) {
                result = voice.renderAddRealtime(
                    interleaved_stereo + sample_offset,
                    chunk_frames);
            } else {
                result = voice.renderAddRealtime(scratch.data(), chunk_frames);
                for (std::size_t index = 0; index < chunk_samples; ++index) {
                    const std::int32_t value = scratch[index];
                    scratch[index] = 0;
                    interleaved_stereo[sample_offset + index] += value;
                    if (shared_send_scale) {
                        const std::int32_t scaled = scaleSend(value, reverb_units);
                        reverb_send_stereo[sample_offset + index] += scaled;
                        chorus_send_stereo[sample_offset + index] += scaled;
                    } else {
                        if (reverb_active) {
                            reverb_send_stereo[sample_offset + index] +=
                                scaleSend(value, reverb_units);
                        }
                        if (chorus_active) {
                            chorus_send_stereo[sample_offset + index] +=
                                scaleSend(value, chorus_units);
                        }
                    }
                }
            }

            util::saturatingAdd(stats_.rendered_voice_frames, result.frames_rendered);
            if (result.finished) {
                util::saturatingIncrement(stats_.voices_finished);
            }
        }

        frame_offset += chunk_frames;
    }

    stats_.active_voices = activeVoiceCount();
    refreshTailStats();
}

}  // namespace gm_synth::voice
