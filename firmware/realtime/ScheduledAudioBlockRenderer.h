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

struct ScheduledWorstBlockEventContext {
    std::uint16_t event_channel_mask = 0;
    std::uint16_t sustain_channel_mask = 0;
    std::uint8_t total = 0;
    std::uint8_t note_on = 0;
    std::uint8_t note_off = 0;
    std::uint8_t control_change = 0;
    std::uint8_t program_change = 0;
    std::uint8_t pitch_bend = 0;
    std::uint8_t channel_pressure = 0;
    std::uint8_t gm_reset = 0;
};

struct ScheduledWorstBlockVoiceContext {
    std::uint16_t channel_mask = 0;
    std::uint8_t active = 0;
    std::uint8_t releasing = 0;
    std::uint8_t lowpass = 0;
    std::uint8_t vibrato = 0;
    std::uint8_t mod_lfo = 0;
    std::uint8_t mod_env = 0;
    std::uint8_t fx_send = 0;
    std::uint8_t realtime_control = 0;
    std::uint8_t fast_release_tails = 0;
    std::uint8_t steal_tails = 0;
    std::uint8_t exclusive_tails = 0;
};

struct ScheduledWorstBlockEntry {
    bool valid = false;
    std::size_t stream_block = 0;
    std::uint64_t block_start_frame = 0;
    std::uint32_t render_us = 0;
    ScheduledWorstBlockEventContext events{};
    ScheduledWorstBlockVoiceContext voices{};
};

struct ScheduledWorstBlockProfile {
    static constexpr std::size_t kCapacity = 16;

    std::uint32_t deadline_us = 0;
    std::uint32_t deadline_candidates = 0;
    std::size_t retained = 0;
    std::array<ScheduledWorstBlockEntry, kCapacity> slowest{};
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

    // Physical qualification diagnostic. Profiling is opt-in and normally off,
    // so production/QEMU rendering keeps the previous hot path. On ESP targets
    // elapsed time is captured around the pure renderer. Voice/event snapshots
    // are taken only for deadline misses that enter the retained Top-N.
    void beginWorstBlockProfiling(
        std::uint32_t deadline_us,
        std::uint64_t first_block_start_frame);
    void endWorstBlockProfiling() { worst_profile_enabled_ = false; }
    const ScheduledWorstBlockProfile& worstBlockProfile() const {
        return worst_profile_;
    }

private:
    static std::int16_t saturateToI16(std::int32_t sample, bool* clipped);
    static bool inspectProfileVoice(void* context, voice::Voice& voice);
    static std::uint8_t saturatingProfileCount(std::size_t value);

    void renderSegment(std::size_t start_frame, std::size_t frame_count);
    void finalize(std::int16_t* interleaved_stereo);
    void recordWorstBlock(
        std::uint64_t block_start_frame,
        std::uint32_t render_us,
        const BlockEventBatch& batch);
    ScheduledWorstBlockVoiceContext captureProfileVoices();
    void insertWorstBlock(ScheduledWorstBlockEntry entry);

    voice::VoicePool& voices_;
    BlockEventScheduler& scheduler_;
    RealtimeEventApplyFn apply_event_ = nullptr;
    void* apply_context_ = nullptr;
    std::array<std::int32_t, kSamplesPerBlock> mix_{};
    std::array<std::int32_t, kSamplesPerBlock> reverb_send_{};
    std::array<std::int32_t, kSamplesPerBlock> chorus_send_{};
    voice::GlobalEffectsProcessor effects_{};
    ScheduledAudioStats stats_{};

    bool worst_profile_enabled_ = false;
    std::uint64_t worst_profile_first_frame_ = 0;
    std::uint16_t worst_profile_sustain_mask_ = 0;
    ScheduledWorstBlockProfile worst_profile_{};
};

}  // namespace gm_synth::realtime
