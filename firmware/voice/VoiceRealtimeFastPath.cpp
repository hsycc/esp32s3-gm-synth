#include "VoiceRenderer.h"

namespace gm_synth::voice {
namespace {

std::int32_t scaleSampleQ15Fast(std::int32_t sample, std::int32_t gain_q15) {
    if (gain_q15 >= kQ15Unity) {
        return sample;
    }
    if (gain_q15 <= 0) {
        return 0;
    }

    const std::int64_t product = static_cast<std::int64_t>(sample) * gain_q15;
    if (product >= 0) {
        return static_cast<std::int32_t>((product + (std::int64_t{1} << 14)) >> 15);
    }
    return -static_cast<std::int32_t>(((-product) + (std::int64_t{1} << 14)) >> 15);
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

        interleaved_stereo[frame * 2u] += scaleSampleQ15Fast(mono, left_gain);
        interleaved_stereo[frame * 2u + 1u] += scaleSampleQ15Fast(mono, right_gain);
        ++result.frames_rendered;

        phase += phase_increment;
        if (phase >= loop_end_q32) {
            phase = loop_start_q32 + ((phase - loop_start_q32) % loop_length_q32);
        }
    }

    if (active_) {
        phase_q32_ = phase;
    }
    result.finished = !active_;
    return result;
}

}  // namespace gm_synth::voice
