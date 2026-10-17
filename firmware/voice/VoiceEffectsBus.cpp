#include "VoiceRenderer.h"

#include "util/SaturatingCounter.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>

namespace gm_synth::voice {
namespace {

constexpr std::size_t kEffectsScratchFrames = 64;
constexpr std::size_t kChannels = 2;

std::int32_t scaleSend(std::int32_t sample, std::int32_t units) {
    const std::int32_t clamped = std::clamp<std::int32_t>(units, 0, kMaxEffectsSendUnits);
    if (sample == 0 || clamped == 0) {
        return 0;
    }
    const std::int64_t product = static_cast<std::int64_t>(sample) * clamped;
    return product >= 0
        ? static_cast<std::int32_t>((product + kMaxEffectsSendUnits / 2) /
                                    kMaxEffectsSendUnits)
        : -static_cast<std::int32_t>(((-product) + kMaxEffectsSendUnits / 2) /
                                     kMaxEffectsSendUnits);
}

std::int32_t scaleSampleQ15Fast(std::int32_t sample, std::int32_t gain_q15) {
    if (gain_q15 >= kQ15Unity) {
        return sample;
    }
    if (gain_q15 <= 0) {
        return 0;
    }

    // The fast path interpolates int16 PCM, so sample remains in
    // [-32768, 32767]. gain_q15 is clamped to [0, 32767]. Their product is
    // therefore safely inside int32; using a 64-bit multiply here only adds
    // cost on Xtensa and does not provide extra numerical precision.
    const std::int32_t product = sample * gain_q15;
    if (product >= 0) {
        return (product + (std::int32_t{1} << 14)) >> 15;
    }
    return -((-product + (std::int32_t{1} << 14)) >> 15);
}

}  // namespace

VoiceRenderResult Voice::renderAddRealtime(
    std::int32_t* interleaved_stereo,
    std::size_t frame_count) {
    if (!active_ || interleaved_stereo == nullptr || frame_count == 0) {
        VoiceRenderResult result{};
        result.finished = !active_;
        return result;
    }

    // Specialize only the invariant steady-state case. Attack/decay/release,
    // modulation, filtering and non-continuous playback remain on renderAdd(),
    // preserving the generic renderer as the reference implementation.
    const bool steady_sustain =
        envelope_stage_ == EnvelopeStage::Sustain &&
        !key_released_ &&
        params_.loop_mode == LoopMode::Continuous &&
        !params_.vibrato_lfo.enabled &&
        !params_.mod_lfo.enabled &&
        !params_.mod_envelope.enabled &&
        !params_.dynamic_lowpass_table.enabled &&
        !current_lowpass_.enabled;
    if (!steady_sustain || !ref_.valid() || ref_.record == nullptr ||
        ref_.record->pcm == nullptr) {
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
    const std::uint64_t loop_length_q32 = loop_end_q32 - loop_start_q32;
    if (loop_length_q32 == 0) {
        return renderAdd(interleaved_stereo, frame_count);
    }

    std::uint64_t phase = phase_q32_;
    const std::uint64_t phase_increment = current_phase_increment_q32_;
    const std::int32_t envelope = params_.envelope.sustain_q15;
    envelope_level_q15_ = envelope;
    const std::int32_t left_gain = multiplyQ15(current_gain_left_q15_, envelope);
    const std::int32_t right_gain = multiplyQ15(current_gain_right_q15_, envelope);
    std::int32_t* output = interleaved_stereo;

    VoiceRenderResult result{};
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

        output[0] += scaleSampleQ15Fast(mono, left_gain);
        output[1] += scaleSampleQ15Fast(mono, right_gain);
        output += kChannels;
        ++result.frames_rendered;

        phase += phase_increment;
        if (phase >= loop_end_q32) {
            // The normal musical case advances across at most one loop edge.
            // Avoid 64-bit modulo there; preserve the original modulo formula
            // as an exact fallback for extreme increments spanning 2+ loops.
            const std::uint64_t overshoot = phase - loop_end_q32;
            if (overshoot < loop_length_q32) {
                phase = loop_start_q32 + overshoot;
            } else {
                phase = loop_start_q32 +
                    ((phase - loop_start_q32) % loop_length_q32);
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
            const bool needs_send_scratch =
                (reverb_send_stereo != nullptr && reverb_units != 0) ||
                (chorus_send_stereo != nullptr && chorus_units != 0);

            VoiceRenderResult result{};
            if (!needs_send_scratch) {
                result = voice.renderAddRealtime(
                    interleaved_stereo + sample_offset,
                    chunk_frames);
            } else {
                std::fill_n(scratch.data(), chunk_samples, 0);
                result = voice.renderAddRealtime(scratch.data(), chunk_frames);
                for (std::size_t index = 0; index < chunk_samples; ++index) {
                    const std::int32_t value = scratch[index];
                    interleaved_stereo[sample_offset + index] += value;
                    if (reverb_send_stereo != nullptr && reverb_units != 0) {
                        reverb_send_stereo[sample_offset + index] +=
                            scaleSend(value, reverb_units);
                    }
                    if (chorus_send_stereo != nullptr && chorus_units != 0) {
                        chorus_send_stereo[sample_offset + index] +=
                            scaleSend(value, chorus_units);
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
