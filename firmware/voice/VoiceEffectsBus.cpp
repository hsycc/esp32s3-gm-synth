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

}  // namespace

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
                result = voice.renderAdd(
                    interleaved_stereo + sample_offset,
                    chunk_frames);
            } else {
                std::fill_n(scratch.data(), chunk_samples, 0);
                result = voice.renderAdd(scratch.data(), chunk_frames);
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
