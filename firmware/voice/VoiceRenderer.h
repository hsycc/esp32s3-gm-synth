#pragma once

#include "sample/PublishedSampleDirectory.h"

#include <array>
#include <cstddef>
#include <cstdint>

namespace gm_synth::voice {

constexpr std::int32_t kQ15Unity = 32767;
constexpr std::uint64_t kPhaseOneQ32 = std::uint64_t{1} << 32;
constexpr std::int32_t kMaxEffectsSendUnits = 1000;
constexpr std::uint32_t kFilterCoefficientFractionBits = 24;
constexpr std::size_t kLfoControlPointCount = 9;

struct BiquadLowpassConfig {
    bool enabled = false;
    // Transposed-direct-form-II coefficients in signed Q24. The low-pass is
    // symmetric, so a2 == a0 and need not be stored separately.
    std::int32_t a0_q24 = 0;
    std::int32_t a1_q24 = 0;
    std::int32_t b1_q24 = 0;
    std::int32_t b2_q24 = 0;

    bool valid() const {
        return !enabled || (a0_q24 > 0 && a1_q24 >= 0);
    }
};

struct StereoGainQ15 {
    std::int32_t left = kQ15Unity;
    std::int32_t right = kQ15Unity;

    bool valid() const {
        return left >= 0 && left <= kQ15Unity && right >= 0 && right <= kQ15Unity;
    }
};

// Nine uniformly-spaced points cover LFO source levels -1.0 .. +1.0. The GM
// layer prepares the exact attenuation/pan result at those points; Voice only
// performs bounded linear interpolation at the control tick.
struct LfoGainTable {
    bool enabled = false;
    std::array<StereoGainQ15, kLfoControlPointCount> points{};

    bool valid() const {
        if (!enabled) {
            return true;
        }
        for (const auto& point : points) {
            if (!point.valid()) {
                return false;
            }
        }
        return true;
    }
};

// Dynamic filter cutoff is additive in cents across Mod-LFO and Mod-Envelope.
// NoteOn therefore prepares one table over the reachable total offset range,
// rather than one table per source whose coefficients would overwrite each
// other. Voice only sums source cents and linearly interpolates Q24 coefficients.
struct DynamicLowpassTable {
    bool enabled = false;
    std::int32_t min_offset_cents = 0;
    std::int32_t max_offset_cents = 0;
    std::array<BiquadLowpassConfig, kLfoControlPointCount> points{};

    bool valid() const {
        if (!enabled) {
            return true;
        }
        if (min_offset_cents > max_offset_cents) {
            return false;
        }
        for (const auto& point : points) {
            if (!point.valid() || !point.enabled) {
                return false;
            }
        }
        return true;
    }
};

// Fixed-point triangle LFO prepared entirely outside the per-frame hot path.
// phase_step_q32 is cycles-per-output-frame in Q0.32. pitch_depth_cents is the
// current peak pitch depth; channel controllers may update it while sounding.
struct PitchLfoConfig {
    bool enabled = false;
    std::uint32_t delay_frames = 0;
    std::uint32_t phase_step_q32 = 0;
    std::int32_t pitch_depth_cents = 0;

    bool valid() const {
        return !enabled || phase_step_q32 != 0;
    }
};

// SoundFont Mod-LFO has one oscillator and may target pitch, filter cutoff and
// volume simultaneously. Gain nonlinearity is baked into the nine-point table;
// filter depth is combined with Mod-Envelope before using DynamicLowpassTable.
struct ModLfoConfig {
    bool enabled = false;
    std::uint32_t delay_frames = 0;
    std::uint32_t phase_step_q32 = 0;
    std::int32_t pitch_depth_cents = 0;
    std::int32_t filter_depth_cents = 0;
    LfoGainTable gain_table{};

    bool valid() const {
        return !enabled || (phase_step_q32 != 0 && gain_table.valid());
    }
};

enum class LoopMode : std::uint8_t {
    None = 0,
    Continuous,
    UntilRelease,
};

struct EnvelopeConfig {
    std::uint32_t delay_frames = 0;
    std::uint32_t attack_frames = 0;
    std::uint32_t hold_frames = 0;
    std::uint32_t decay_frames = 0;
    std::int32_t sustain_q15 = kQ15Unity;
    std::uint32_t release_frames = 64;
};

// Independent unipolar SF2 modulation envelope. Attack is rendered with the
// SF2 convex shape; hold/decay/sustain/release use the linear envelope value.
struct ModEnvelopeConfig {
    bool enabled = false;
    EnvelopeConfig envelope{};
    std::int32_t pitch_depth_cents = 0;
    std::int32_t filter_depth_cents = 0;

    bool valid() const {
        return !enabled ||
            (envelope.sustain_q15 >= 0 && envelope.sustain_q15 <= kQ15Unity);
    }
};

// Optional per-zone playback window within one resident shdr sample. SoundFont
// address generators are resolved in the control domain into this relative
// frame region. When enabled=false the Voice uses the sample-global descriptor
// exactly as before, preserving the existing direct VoiceStartParams API.
struct SampleRegion {
    bool enabled = false;
    std::uint32_t start_frame = 0;
    std::uint32_t end_frame = 0;  // exclusive
    std::uint32_t loop_start_frame = 0;
    std::uint32_t loop_end_frame = 0;  // exclusive
};

// Opaque realtime-control source copied into a Voice at NoteOn. VoiceRenderer
// never interprets SoundFont/MIDI semantics from these fields; the GM layer
// owns the transforms and writes back a final VoiceRuntimeControlUpdate. This
// keeps dependency direction GM/SF2 -> Voice while still allowing already-
// sounding Voices to react sample-accurately to channel controller events.
struct VoiceRealtimeControlConfig {
    bool enabled = false;
    bool gain_enabled = false;
    bool effects_send_enabled = false;
    bool vibrato_enabled = false;
    bool mod_lfo_gain_enabled = false;
    std::uint64_t unbent_phase_increment_q32 = kPhaseOneQ32;
    std::int32_t initial_attenuation_cb = 0;
    std::int32_t velocity_to_attenuation_cb = 0;
    std::int32_t volume_to_attenuation_cb = 0;
    std::int32_t pan_amount = 0;
    std::int32_t expression_to_attenuation_cb = 0;
    std::int16_t pan_sf2_units = 0;
    std::int16_t reverb_send_base_units = 0;
    std::int16_t chorus_send_base_units = 0;
    std::int32_t reverb_send_cc_amount = 0;
    std::int32_t chorus_send_cc_amount = 0;
    std::int32_t static_vibrato_pitch_depth_cents = 0;
    std::int32_t channel_pressure_to_vibrato_pitch_cents = 0;
    std::int32_t modulation_wheel_to_vibrato_pitch_cents = 0;
    std::int32_t mod_lfo_volume_depth_cb = 0;
};

struct VoiceRuntimeControlUpdate {
    std::uint64_t phase_increment_q32 = kPhaseOneQ32;
    std::int32_t gain_left_q15 = kQ15Unity;
    std::int32_t gain_right_q15 = kQ15Unity;
    std::int16_t reverb_send_units = 0;
    std::int16_t chorus_send_units = 0;
    std::int32_t vibrato_pitch_depth_cents = 0;
    LfoGainTable mod_lfo_gain_table{};

    bool valid() const {
        return phase_increment_q32 != 0 && mod_lfo_gain_table.valid() &&
            reverb_send_units >= 0 && reverb_send_units <= kMaxEffectsSendUnits &&
            chorus_send_units >= 0 && chorus_send_units <= kMaxEffectsSendUnits;
    }
};

struct VoiceStartParams {
    sample::SampleId sample_id = 0;
    std::uint8_t channel = 0;
    std::uint8_t key = 60;
    std::uint8_t velocity = 100;
    // SoundFont exclusiveClass. Zero means no exclusive-class behavior.
    std::uint16_t exclusive_class = 0;
    // All layers spawned by one logical NoteOn should share the same non-zero
    // ID so they do not exclusive-choke each other. Zero means "un-grouped";
    // every start is then treated as an independent trigger.
    std::uint32_t note_instance_id = 0;
    std::uint64_t phase_increment_q32 = kPhaseOneQ32;
    LoopMode loop_mode = LoopMode::None;
    SampleRegion sample_region{};
    BiquadLowpassConfig lowpass{};
    DynamicLowpassTable dynamic_lowpass_table{};
    PitchLfoConfig vibrato_lfo{};
    ModLfoConfig mod_lfo{};
    ModEnvelopeConfig mod_envelope{};
    std::int32_t gain_left_q15 = kQ15Unity;
    std::int32_t gain_right_q15 = kQ15Unity;
    // Prepared send state in SoundFont tenths-of-a-percent units.
    std::int16_t reverb_send_units = 0;
    std::int16_t chorus_send_units = 0;
    EnvelopeConfig envelope{};
    VoiceRealtimeControlConfig realtime_control{};
};

struct VoiceRenderResult {
    std::size_t frames_rendered = 0;
    bool finished = false;
};

struct VoiceStealConfig {
    // Controls overload-driven victim stealing only. SoundFont exclusive-class
    // choke remains active when enabled=false.
    bool enabled = false;
    // The fade and transient-tail budget are shared by overload stealing and
    // exclusive-class choke so the two policies cannot create unbounded tails.
    std::uint32_t fade_frames = 32;
    std::size_t max_tail_voices = 2;
};

struct VoicePoolStats {
    std::uint64_t start_attempts = 0;
    std::uint64_t start_successes = 0;
    std::uint64_t start_failures_rejected = 0;
    std::uint64_t start_failures_full = 0;
    std::uint64_t start_failures_admission = 0;
    std::uint64_t note_off_matches = 0;
    std::uint64_t sustain_deferred_note_offs = 0;
    std::uint64_t sustain_release_voices = 0;
    std::uint64_t all_notes_off_matches = 0;
    std::uint64_t channel_sound_off_voices = 0;
    std::uint64_t runtime_control_updates = 0;
    std::uint64_t voices_finished = 0;
    std::uint64_t rendered_voice_frames = 0;
    std::uint64_t steal_attempts = 0;
    std::uint64_t steal_successes = 0;
    std::uint64_t steal_failures_tail_budget = 0;
    std::uint64_t steal_failures_over_target = 0;
    std::uint64_t steal_failures_no_victim = 0;
    std::uint64_t steals_from_release = 0;
    std::uint64_t steals_from_active = 0;
    std::uint64_t exclusive_choke_attempts = 0;
    std::uint64_t exclusive_choke_successes = 0;
    std::uint64_t exclusive_choke_failures_tail_budget = 0;
    std::uint64_t exclusive_choke_failures_over_target = 0;
    std::uint64_t exclusive_voices_released = 0;
    std::size_t active_voices = 0;
    std::size_t peak_active_voices = 0;
    std::size_t admission_limit = 0;
    std::size_t active_steal_tails = 0;
    std::size_t active_exclusive_tails = 0;
    std::size_t active_fast_release_tails = 0;
    std::uint8_t last_stolen_channel = 0;
    std::uint8_t last_stolen_key = 0;
    std::uint16_t last_exclusive_class = 0;
};

class Voice {
public:
    Voice() = default;
    Voice(const Voice&) = delete;
    Voice& operator=(const Voice&) = delete;

    bool start(sample::PublishedSampleDirectory& directory, const VoiceStartParams& params);
    void noteOff();
    bool beginFastRelease(std::uint32_t fade_frames);
    bool beginExclusiveRelease(std::uint32_t fade_frames);
    bool applyRuntimeControlUpdate(const VoiceRuntimeControlUpdate& update);
    VoiceRenderResult renderAdd(std::int32_t* interleaved_stereo, std::size_t frame_count);
    // Realtime path: takes a bit-equivalent steady-Sustain fast path when safe
    // and otherwise falls back to renderAdd(), which remains the reference path.
    VoiceRenderResult renderAddRealtime(
        std::int32_t* interleaved_stereo,
        std::size_t frame_count);
    void reset();

    bool active() const { return active_; }
    bool releasing() const;
    bool stealTail() const;
    bool exclusiveTail() const;
    bool fastReleaseTail() const;
    std::uint8_t channel() const { return channel_; }
    std::uint8_t key() const { return key_; }
    std::uint8_t velocity() const { return params_.velocity; }
    std::uint16_t exclusiveClass() const { return params_.exclusive_class; }
    std::uint32_t noteInstanceId() const { return params_.note_instance_id; }
    sample::SampleId sampleId() const { return ref_.sample_id; }
    std::uint64_t phaseQ32() const { return phase_q32_; }
    std::uint64_t phaseIncrementQ32() const { return params_.phase_increment_q32; }
    std::uint64_t currentPhaseIncrementQ32() const { return current_phase_increment_q32_; }
    std::int32_t gainLeftQ15() const { return params_.gain_left_q15; }
    std::int32_t gainRightQ15() const { return params_.gain_right_q15; }
    std::int32_t currentGainLeftQ15() const { return current_gain_left_q15_; }
    std::int32_t currentGainRightQ15() const { return current_gain_right_q15_; }
    std::int16_t reverbSendUnits() const { return params_.reverb_send_units; }
    std::int16_t chorusSendUnits() const { return params_.chorus_send_units; }
    std::int32_t envelopeLevelQ15() const { return envelope_level_q15_; }
    std::int32_t modEnvelopeLevelQ15() const { return mod_envelope_level_q15_; }
    std::int32_t modEnvelopeDspLevelQ15() const;
    bool lowpassEnabled() const { return current_lowpass_.enabled; }
    const BiquadLowpassConfig& lowpassConfig() const { return params_.lowpass; }
    const BiquadLowpassConfig& currentLowpassConfig() const { return current_lowpass_; }
    const DynamicLowpassTable& dynamicLowpassTable() const {
        return params_.dynamic_lowpass_table;
    }
    const PitchLfoConfig& vibratoLfoConfig() const { return params_.vibrato_lfo; }
    const ModLfoConfig& modLfoConfig() const { return params_.mod_lfo; }
    const ModEnvelopeConfig& modEnvelopeConfig() const { return params_.mod_envelope; }
    const VoiceRealtimeControlConfig& realtimeControl() const {
        return params_.realtime_control;
    }

private:
    enum class EnvelopeStage : std::uint8_t {
        Delay = 0,
        Attack,
        Hold,
        Decay,
        Sustain,
        Release,
        Done,
    };

    enum class FastReleaseKind : std::uint8_t {
        None = 0,
        VoiceSteal,
        ExclusiveClass,
    };

    static std::int32_t clampQ15(std::int32_t value);
    static std::int32_t multiplyQ15(std::int32_t a, std::int32_t b);

    std::uint32_t playbackStartFrame() const;
    std::uint32_t playbackEndFrame() const;
    std::uint32_t playbackLoopStartFrame() const;
    std::uint32_t playbackLoopEndFrame() const;
    bool validLoop() const;
    bool loopActive() const;
    std::int32_t nextEnvelopeLevel();
    std::int32_t nextModEnvelopeLevel();
    std::int32_t interpolatedSample() const;
    std::int32_t applyLowpass(std::int32_t sample);
    void refreshModulationState();
    void advanceLfos();
    void beginModEnvelopeRelease();
    void advancePhase();
    void beginRelease(std::uint32_t release_frames, FastReleaseKind fast_release_kind);
    void finish();

    sample::PublishedSampleDirectory* directory_ = nullptr;
    sample::PublishedVoiceRef ref_{};
    VoiceStartParams params_{};

    bool active_ = false;
    bool key_released_ = false;
    FastReleaseKind fast_release_kind_ = FastReleaseKind::None;
    std::uint8_t channel_ = 0;
    std::uint8_t key_ = 0;
    std::uint64_t phase_q32_ = 0;
    std::uint64_t current_phase_increment_q32_ = kPhaseOneQ32;
    std::int32_t current_gain_left_q15_ = kQ15Unity;
    std::int32_t current_gain_right_q15_ = kQ15Unity;
    BiquadLowpassConfig current_lowpass_{};
    std::uint32_t vibrato_phase_q32_ = 0;
    std::uint32_t vibrato_delay_remaining_ = 0;
    std::uint8_t vibrato_control_frames_remaining_ = 0;
    std::uint32_t mod_lfo_phase_q32_ = 0;
    std::uint32_t mod_lfo_delay_remaining_ = 0;
    std::uint8_t mod_lfo_control_frames_remaining_ = 0;
    std::uint8_t mod_envelope_control_frames_remaining_ = 0;
    std::int64_t lowpass_z1_q24_ = 0;
    std::int64_t lowpass_z2_q24_ = 0;
    EnvelopeStage envelope_stage_ = EnvelopeStage::Done;
    std::uint32_t envelope_stage_frame_ = 0;
    std::uint32_t active_release_frames_ = 0;
    std::int32_t envelope_level_q15_ = 0;
    std::int32_t release_start_level_q15_ = 0;
    EnvelopeStage mod_envelope_stage_ = EnvelopeStage::Done;
    std::uint32_t mod_envelope_stage_frame_ = 0;
    std::uint32_t mod_envelope_release_frames_ = 0;
    std::int32_t mod_envelope_level_q15_ = 0;
    std::int32_t mod_envelope_release_start_level_q15_ = 0;
};

using ActiveVoiceUpdater = bool (*)(void* context, Voice& voice);

class VoicePool {
public:
    static constexpr std::size_t kHardVoiceCap = 32;

    explicit VoicePool(sample::PublishedSampleDirectory& directory)
        : directory_(directory) {
        stats_.admission_limit = admission_limit_;
    }

    bool start(const VoiceStartParams& params);
    std::size_t noteOff(std::uint8_t channel, std::uint8_t key);
    // GM/MIDI path: when sustain is down, physical NoteOff is remembered for
    // this exact slot-start serial and released only when the pedal comes up.
    std::size_t noteOff(std::uint8_t channel, std::uint8_t key, bool sustain_down);
    std::size_t releaseSustain(std::uint8_t channel);
    std::size_t allNotesOff(std::uint8_t channel, bool sustain_down);
    void renderAdd(std::int32_t* interleaved_stereo, std::size_t frame_count);
    // Renders each Voice once and derives independent SoundFont reverb/chorus
    // send buses from that Voice's post-filter/post-envelope/post-pan signal.
    // Send values are read after every sub-block boundary, so realtime CC91/93
    // changes applied by ScheduledAudioBlockRenderer affect the next segment.
    void renderAddWithEffects(
        std::int32_t* interleaved_stereo,
        std::int32_t* reverb_send_stereo,
        std::int32_t* chorus_send_stereo,
        std::size_t frame_count);
    void allSoundOff();
    std::size_t allSoundOff(std::uint8_t channel);

    // Fixed-cap bounded visitor used by the GM layer to recompute controller
    // state for already-sounding Voices. No allocation, locks, or container
    // mutation occurs while the visitor runs.
    std::size_t updateActiveVoices(
        std::uint8_t channel,
        ActiveVoiceUpdater updater,
        void* context);

    void setAdmissionLimit(std::size_t limit);
    std::size_t admissionLimit() const { return admission_limit_; }
    void setVoiceStealConfig(const VoiceStealConfig& config);
    const VoiceStealConfig& voiceStealConfig() const { return steal_config_; }
    std::size_t activeVoiceCount() const;
    std::size_t activeStealTailCount() const;
    std::size_t activeExclusiveTailCount() const;
    std::size_t activeFastReleaseTailCount() const;
    const VoicePoolStats& stats() const { return stats_; }

private:
    std::size_t findInactiveSlot() const;
    std::size_t chooseStealVictim() const;
    std::size_t exclusiveVictimCount(const VoiceStartParams& params) const;
    std::size_t releaseExclusiveVictims(
        const VoiceStartParams& params,
        std::size_t incoming_slot);
    void recordStartedVoice(std::size_t slot, std::size_t active_before);
    void refreshTailStats();

    sample::PublishedSampleDirectory& directory_;
    std::array<Voice, kHardVoiceCap> voices_{};
    std::array<std::uint64_t, kHardVoiceCap> start_serials_{};
    // Zero means no deferred sustain NoteOff. A saved serial is valid only if
    // it still equals start_serials_[slot], preventing stale pedal state from a
    // finished/reused slot from touching a later Voice.
    std::array<std::uint64_t, kHardVoiceCap> sustain_pending_serials_{};
    std::uint64_t next_start_serial_ = 1;
    std::size_t admission_limit_ = kHardVoiceCap;
    VoiceStealConfig steal_config_{};
    VoicePoolStats stats_{};
};

}  // namespace gm_synth::voice
