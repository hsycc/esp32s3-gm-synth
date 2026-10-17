#pragma once

#include "BlockEventScheduler.h"
#include "voice/AudioBlockRenderer.h"

#include <array>
#include <cstddef>
#include <cstdint>

namespace gm_synth::realtime {

using RealtimeEventApplyFn = bool (*)(
    void* context,
    const RealtimeEvent& event,
    voice::VoicePool& voices);

struct ScheduledAudioStats {
    std::uint64_t blocks_rendered = 0;
    std::uint64_t output_frames = 0;
    std::uint64_t render_segments = 0;
    std::uint64_t events_attempted = 0;
    std::uint64_t events_applied = 0;
    std::uint64_t events_rejected = 0;
    std::uint64_t clipped_samples = 0;
    std::int32_t peak_abs_accumulator = 0;
};

// M5 audio-side block renderer. It consumes a prepared timestamp batch and
// renders only the ranges between event offsets, applying events exactly at the
// requested sample boundary. Dry audio and SF2 effects sends are segmented by
// the same boundaries; global effects are then processed once over the complete
// block. No allocation, sorting, storage I/O or blocking occurs here.
class ScheduledAudioBlockRenderer {
public:
    static constexpr std::size_t kFramesPerBlock = voice::AudioBlockRenderer::kFramesPerBlock;
    static constexpr std::size_t kChannels = voice::AudioBlockRenderer::kChannels;
    static constexpr std::size_t kSamplesPerBlock = kFramesPerBlock * kChannels;

    ScheduledAudioBlockRenderer(
        voice::VoicePool& voices,
        BlockEventScheduler& scheduler,
        RealtimeEventApplyFn apply_event,
        void* apply_context = nullptr)
        : voices_(voices),
          scheduler_(scheduler),
          apply_event_(apply_event),
          apply_context_(apply_context) {}

    void render(std::uint64_t block_start_frame, std::int16_t* interleaved_stereo);

    const ScheduledAudioStats& stats() const { return stats_; }

private:
    static std::int16_t saturateToI16(std::int32_t sample, bool* clipped);
    void renderSegment(std::size_t start_frame, std::size_t frame_count);
    void finalize(std::int16_t* interleaved_stereo);

    voice::VoicePool& voices_;
    BlockEventScheduler& scheduler_;
    RealtimeEventApplyFn apply_event_ = nullptr;
    void* apply_context_ = nullptr;
    std::array<std::int32_t, kSamplesPerBlock> mix_{};
    std::array<std::int32_t, kSamplesPerBlock> reverb_send_{};
    std::array<std::int32_t, kSamplesPerBlock> chorus_send_{};
    voice::GlobalEffectsProcessor effects_{};
    ScheduledAudioStats stats_{};
};

}  // namespace gm_synth::realtime
