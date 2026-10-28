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

// Only event-heavy physical blocks (>=8 scheduled events) get stage timing.
struct ScheduledWorstBlockStages {
    bool valid = false;
    std::uint32_t setup_prepare_us = 0;
    std::uint32_t event_apply_us = 0;
    // Optional ESP-only per-event timings within the event-apply stage;
    // recording them adds clock calls and must never be treated as the
    // non-instrumented production event cost.
    std::uint32_t note_on_apply_us = 0;
    std::uint32_t cc120_apply_us = 0;
    std::uint32_t other_cc_apply_us = 0;
    std::uint32_t other_event_apply_us = 0;
    std::uint32_t voice_render_us = 0;
    std::uint32_t effects_finalize_us = 0;
};

struct ScheduledWorstBlockEntry {
    bool valid = false;
    std::size_t stream_block = 0;
    std::uint64_t block_start_frame = 0;
    std::uint32_t render_us = 0;
    ScheduledWorstBlockStages stages{};
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
    // BEGIN V1_OUTPUT_FINALIZER
#if GM_SYNTH_EXPERIMENTAL_VOICE_LOOP
    // Optional application output stage after FX, before PCM conversion. The
    // callback owns master scaling and conversion and updates output-only stats.
    // Configure before the first block, then retain its context for the stream.
    using OutputFinalizer = void (*)(void*, const std::int32_t*, std::size_t,
        std::int32_t, std::int16_t*, ScheduledAudioStats&) noexcept;
    bool setOutputFinalizerQuiescent(OutputFinalizer fn, void* context) noexcept {
        if (stats_.blocks_rendered || (fn && !context)) return false;
        output_finalizer_ = fn; output_context_ = context;
        return true;
    }
#endif
    // END V1_OUTPUT_FINALIZER

    // Global digital post-FX master. Default is exact bypass so all existing
    // SF2/MIDI/qualification PCM output remains bit-identical. 0=mute,
    // 32767=unity; no amplification or uncontrolled clipping is allowed here.
    // Set from the control/audio owner before rendering, not concurrently with
    // render(). CC7/CC11 remain independent *per-channel* controllers.
    bool setMasterGainQ15(std::int32_t gain_q15) {
        if (gain_q15 < 0 || gain_q15 > voice::kQ15Unity) {
            return false;
        }
        master_gain_q15_ = gain_q15;
        return true;
    }
    std::int32_t masterGainQ15() const { return master_gain_q15_; }

    // Physical qualification diagnostic. Profiling is opt-in and normally off,
    // so production/QEMU rendering keeps the previous hot path. On ESP targets
    // elapsed time is captured around the pure renderer. Voice/event snapshots
    // are taken only for deadline misses that enter the retained Top-N.
    void beginWorstBlockProfiling(
        std::uint32_t deadline_us,
        std::uint64_t first_block_start_frame);
    void endWorstBlockProfiling() {
        worst_profile_enabled_ = false;
        last_profiled_render_us_ = 0;
    }
    bool worstBlockProfilingEnabled() const { return worst_profile_enabled_; }
    std::uint32_t lastProfiledRenderUs() const { return last_profiled_render_us_; }
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
        const BlockEventBatch& batch,
        const ScheduledWorstBlockStages& stages);
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
    std::int32_t master_gain_q15_ = voice::kQ15Unity;
    // BEGIN V1_OUTPUT_FINALIZER
#if GM_SYNTH_EXPERIMENTAL_VOICE_LOOP
    OutputFinalizer output_finalizer_ = nullptr;
    void* output_context_ = nullptr;
#endif
    // END V1_OUTPUT_FINALIZER

    bool worst_profile_enabled_ = false;
    std::uint32_t last_profiled_render_us_ = 0;
    std::uint64_t worst_profile_first_frame_ = 0;
    std::uint16_t worst_profile_sustain_mask_ = 0;
    ScheduledWorstBlockProfile worst_profile_{};
};

}  // namespace gm_synth::realtime
