#pragma once

#include "sample/PublishedSampleDirectory.h"

#include <array>
#include <cstddef>
#include <cstdint>

namespace gm_synth::voice {

constexpr std::int32_t kQ15Unity = 32767;
constexpr std::uint64_t kPhaseOneQ32 = std::uint64_t{1} << 32;

enum class LoopMode : std::uint8_t {
    None = 0,
    Continuous,
    UntilRelease,
};

struct EnvelopeConfig {
    std::uint32_t attack_frames = 0;
    std::uint32_t decay_frames = 0;
    std::int32_t sustain_q15 = kQ15Unity;
    std::uint32_t release_frames = 64;
};

struct VoiceStartParams {
    sample::SampleId sample_id = 0;
    std::uint8_t channel = 0;
    std::uint8_t key = 60;
    std::uint8_t velocity = 100;
    std::uint64_t phase_increment_q32 = kPhaseOneQ32;
    LoopMode loop_mode = LoopMode::None;
    std::int32_t gain_left_q15 = kQ15Unity;
    std::int32_t gain_right_q15 = kQ15Unity;
    EnvelopeConfig envelope{};
};

struct VoiceRenderResult {
    std::size_t frames_rendered = 0;
    bool finished = false;
};

struct VoicePoolStats {
    std::uint64_t start_attempts = 0;
    std::uint64_t start_successes = 0;
    std::uint64_t start_failures_rejected = 0;
    std::uint64_t start_failures_full = 0;
    std::uint64_t start_failures_admission = 0;
    std::uint64_t note_off_matches = 0;
    std::uint64_t voices_finished = 0;
    std::uint64_t rendered_voice_frames = 0;
    std::size_t active_voices = 0;
    std::size_t peak_active_voices = 0;
    std::size_t admission_limit = 0;
};

class Voice {
public:
    Voice() = default;
    Voice(const Voice&) = delete;
    Voice& operator=(const Voice&) = delete;

    bool start(sample::PublishedSampleDirectory& directory, const VoiceStartParams& params);
    void noteOff();
    VoiceRenderResult renderAdd(std::int32_t* interleaved_stereo, std::size_t frame_count);
    void reset();

    bool active() const { return active_; }
    std::uint8_t channel() const { return channel_; }
    std::uint8_t key() const { return key_; }
    sample::SampleId sampleId() const { return ref_.sample_id; }
    std::uint64_t phaseQ32() const { return phase_q32_; }
    std::int32_t envelopeLevelQ15() const { return envelope_level_q15_; }

private:
    enum class EnvelopeStage : std::uint8_t {
        Attack = 0,
        Decay,
        Sustain,
        Release,
        Done,
    };

    static std::int32_t clampQ15(std::int32_t value);
    static std::int32_t multiplyQ15(std::int32_t a, std::int32_t b);

    bool validLoop() const;
    bool loopActive() const;
    std::int32_t nextEnvelopeLevel();
    std::int32_t interpolatedSample() const;
    void advancePhase();
    void finish();

    sample::PublishedSampleDirectory* directory_ = nullptr;
    sample::PublishedVoiceRef ref_{};
    VoiceStartParams params_{};

    bool active_ = false;
    bool key_released_ = false;
    std::uint8_t channel_ = 0;
    std::uint8_t key_ = 0;
    std::uint64_t phase_q32_ = 0;
    EnvelopeStage envelope_stage_ = EnvelopeStage::Done;
    std::uint32_t envelope_stage_frame_ = 0;
    std::int32_t envelope_level_q15_ = 0;
    std::int32_t release_start_level_q15_ = 0;
};

class VoicePool {
public:
    static constexpr std::size_t kHardVoiceCap = 32;

    explicit VoicePool(sample::PublishedSampleDirectory& directory)
        : directory_(directory) {
        stats_.admission_limit = admission_limit_;
    }

    bool start(const VoiceStartParams& params);
    std::size_t noteOff(std::uint8_t channel, std::uint8_t key);
    void renderAdd(std::int32_t* interleaved_stereo, std::size_t frame_count);
    void allSoundOff();

    void setAdmissionLimit(std::size_t limit);
    std::size_t admissionLimit() const { return admission_limit_; }
    std::size_t activeVoiceCount() const;
    const VoicePoolStats& stats() const { return stats_; }

private:
    sample::PublishedSampleDirectory& directory_;
    std::array<Voice, kHardVoiceCap> voices_{};
    std::size_t admission_limit_ = kHardVoiceCap;
    VoicePoolStats stats_{};
};

}  // namespace gm_synth::voice
