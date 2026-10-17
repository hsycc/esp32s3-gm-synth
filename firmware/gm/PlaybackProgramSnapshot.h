#pragma once

#include "sample/ProgramSnapshot.h"
#include "voice/VoiceRenderer.h"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>

namespace gm_synth::gm {

// Compact parameters whose final value depends on the incoming NoteOn and the
// current MIDI channel controller state. Keeping the raw SoundFont attenuation
// summing node here is important: a negative preset initialAttenuation may
// cancel part of velocity/controller attenuation before the final clamp.
struct NoteOnGainConfig {
    bool enabled = false;
    std::int32_t initial_attenuation_cb = 0;
    std::int32_t velocity_to_attenuation_cb = 0;
    std::int32_t volume_to_attenuation_cb = 960;
    std::int32_t pan_amount = 500;
    std::int32_t expression_to_attenuation_cb = 960;
    // Mono zones remain in the native -500..500 SF2 pan range. Expanded
    // left/right stereo layers add their -500/+500 channel base before live
    // CC10 is summed, so retaining -1000..1000 here is required to preserve
    // the correct clamp history when a later controller moves back toward center.
    std::int16_t pan_sf2_units = 0;
    // -1 means use the incoming NoteOn velocity. SF2 generator 47 may replace
    // the velocity used by note-on modulators without changing zone selection.
    std::int16_t fixed_velocity = -1;

    bool valid() const {
        return !enabled ||
            (pan_sf2_units >= -1000 && pan_sf2_units <= 1000 &&
             fixed_velocity >= -1 && fixed_velocity <= 127);
    }
};

// Static initialFilterFc plus velocity/LFO/envelope cutoff movement and the
// full SF2 initialFilterQ range are represented by the prepared Q24 biquad path.
struct NoteOnFilterConfig {
    bool enabled = false;
    std::int32_t initial_cutoff_cents = 13500;
    std::int32_t velocity_to_cutoff_cents = -2400;
    std::int16_t resonance_cb = 0;

    bool valid() const {
        return !enabled ||
            (initial_cutoff_cents >= 1500 && initial_cutoff_cents <= 13500 &&
             resonance_cb >= 0 && resonance_cb <= 960);
    }
};

struct EnvelopeKeyScalingConfig {
    std::int16_t hold_timecents_per_key = 0;
    std::int16_t decay_timecents_per_key = 0;
};

// Prepared SF2 vibrato-LFO state. delay/frequency are converted in the control
// domain; the realtime side only advances a fixed-point triangle oscillator.
// Static gen6 depth is summed with the live Channel Pressure and CC1 routes.
struct VibratoLfoConfig {
    bool enabled = false;
    std::uint32_t delay_frames = 0;
    std::uint32_t phase_step_q32 = 0;
    std::int32_t static_pitch_depth_cents = 0;
    std::int32_t channel_pressure_to_pitch_cents = 50;
    std::int32_t modulation_wheel_to_pitch_cents = 50;

    bool valid() const {
        return !enabled || phase_step_q32 != 0;
    }
};

// SF2 Mod-LFO uses one triangle oscillator for pitch, filter cutoff and volume.
// Nonlinear gain/filter work is prepared later at NoteOn; these fields preserve
// the native depths and timing in immutable program state.
struct ModLfoConfig {
    bool enabled = false;
    std::uint32_t delay_frames = 0;
    std::uint32_t phase_step_q32 = 0;
    std::int32_t pitch_depth_cents = 0;
    std::int32_t filter_depth_cents = 0;
    std::int32_t volume_depth_cb = 0;

    bool valid() const {
        return !enabled || phase_step_q32 != 0;
    }
};

// SF2 modulation envelope is a second independent DAHDSR. It is unipolar and
// drives pitch/filter destinations only. Hold/decay key scaling is applied at
// NoteOn, mirroring the already-qualified volume-envelope rule around key 60.
struct ModEnvelopeConfig {
    bool enabled = false;
    voice::EnvelopeConfig envelope{};
    EnvelopeKeyScalingConfig key_scaling{};
    std::int32_t pitch_depth_cents = 0;
    std::int32_t filter_depth_cents = 0;

    bool valid() const {
        return !enabled ||
            (envelope.sustain_q15 >= 0 && envelope.sustain_q15 <= voice::kQ15Unity);
    }
};

// Prepared SF2 reverb/chorus generator summing state. The runtime computes the
// current send from the static generator plus CC91/CC93. These values are
// carried all the way into Voice even before the actual FX processors exist so
// controller semantics and future routing do not need another metadata change.
struct EffectsSendConfig {
    bool enabled = false;
    std::int16_t reverb_sf2_units = 0;
    std::int16_t chorus_sf2_units = 0;
    std::int32_t reverb_cc_amount = 200;
    std::int32_t chorus_cc_amount = 200;

    bool valid() const {
        return !enabled ||
            (reverb_sf2_units >= 0 && reverb_sf2_units <= 1000 &&
             chorus_sf2_units >= 0 && chorus_sf2_units <= 1000);
    }
};

// Immutable, control-domain-prepared playback metadata for one SF2 zone.
// Expensive SF2 generator/modulator merging belongs before publication. The
// audio domain only performs bounded key/velocity matching and copies these
// already-resolved values into VoiceStartParams.
struct PlaybackZone {
    sample::SampleId sample_id = 0;
    std::uint8_t key_low = 0;
    std::uint8_t key_high = 127;
    std::uint8_t velocity_low = 0;
    std::uint8_t velocity_high = 127;
    std::uint16_t exclusive_class = 0;

    // root_phase_increment_q32 already includes source/output sample-rate,
    // coarse/fine tuning and sample pitch-correction terms for root_key.
    // scale_tuning_cents_per_key is SF2 generator 56. Runtime NoteOn applies
    // (key-root_key)*scaleTuning using fixed-point cents transposition only.
    // key_tracking=false is used by fixed-key and zero-scale zones.
    std::uint8_t root_key = 60;
    bool key_tracking = true;
    std::uint16_t scale_tuning_cents_per_key = 100;
    std::uint64_t root_phase_increment_q32 = voice::kPhaseOneQ32;

    // SoundFont address generators are resolved relative to the resident shdr
    // sample. Disabled means use the entire sample-global SampleDescriptor.
    voice::SampleRegion sample_region{};
    voice::LoopMode loop_mode = voice::LoopMode::None;
    std::int32_t gain_left_q15 = voice::kQ15Unity;
    std::int32_t gain_right_q15 = voice::kQ15Unity;
    voice::EnvelopeConfig envelope{};
    EnvelopeKeyScalingConfig envelope_key_scaling{};
    NoteOnGainConfig note_on_gain{};
    NoteOnFilterConfig note_on_filter{};
    VibratoLfoConfig vibrato_lfo{};
    ModLfoConfig mod_lfo{};
    ModEnvelopeConfig mod_envelope{};
    EffectsSendConfig effects_send{};

    bool valid() const {
        const bool sample_region_valid = !sample_region.enabled ||
            (sample_region.start_frame < sample_region.end_frame &&
             (loop_mode == voice::LoopMode::None ||
              (sample_region.start_frame <= sample_region.loop_start_frame &&
               sample_region.loop_start_frame < sample_region.loop_end_frame &&
               sample_region.loop_end_frame <= sample_region.end_frame)));
        return key_low <= key_high && key_high <= 127 &&
               velocity_low <= velocity_high && velocity_high <= 127 &&
               root_key <= 127 && scale_tuning_cents_per_key <= 1200 &&
               root_phase_increment_q32 != 0 && sample_region_valid &&
               note_on_gain.valid() && note_on_filter.valid() &&
               vibrato_lfo.valid() && mod_lfo.valid() &&
               mod_envelope.valid() && effects_send.valid();
    }

    bool matches(std::uint8_t key, std::uint8_t velocity) const {
        return key >= key_low && key <= key_high &&
               velocity >= velocity_low && velocity <= velocity_high;
    }
};

struct PlaybackProgramSnapshot {
    // This is a realtime scan bound, not an inline-storage size. The checked-in
    // SF2 analysis manifests are validated against it in CI. GeneralUser's
    // largest currently resolved program has 200 zones, so 256 leaves bounded
    // headroom without inflating every per-channel snapshot object.
    static constexpr std::size_t kMaxZones = 256;

    std::uint8_t channel = 0;
    std::uint16_t bank = 0;
    std::uint8_t program = 0;
    std::uint32_t token = 0;
    std::uint32_t bank_generation = 0;

    // Caller-owned immutable storage. The pointed-to zone array must remain
    // alive and unchanged from READY publication until the snapshot is retired
    // and acknowledged by the control domain.
    const PlaybackZone* zones = nullptr;
    std::uint16_t zone_count = 0;

    bool valid() const {
        if (channel >= 16 || token == 0 || bank_generation == 0 ||
            zone_count > kMaxZones || (zone_count != 0 && zones == nullptr)) {
            return false;
        }
        for (std::size_t i = 0; i < zone_count; ++i) {
            if (!zones[i].valid()) {
                return false;
            }
        }
        return true;
    }
};

struct PlaybackProgramCommitAck {
    std::uint32_t token = 0;
    const PlaybackProgramSnapshot* retired = nullptr;

    bool valid() const { return token != 0; }
};

struct PlaybackProgramExchangeStats {
    std::uint64_t ready_publishes = 0;
    std::uint64_t ready_rejections = 0;
    std::uint64_t commits = 0;
    std::uint64_t commit_acks = 0;
};

// Playback counterpart to sample::ProgramSnapshotExchange. It intentionally
// contains pointers only; zone arrays and snapshots are caller-owned immutable
// objects prepared outside the realtime domain.
class PlaybackProgramSnapshotExchange {
public:
    static constexpr std::size_t kMidiChannels = 16;

    PlaybackProgramSnapshotExchange();

    bool setInitialActive(const PlaybackProgramSnapshot* snapshot);
    bool publishReady(const PlaybackProgramSnapshot* snapshot);
    bool commitAtBlockBoundary(std::uint8_t channel);
    PlaybackProgramCommitAck acknowledgeCommit(std::uint8_t channel, std::uint32_t token);

    const PlaybackProgramSnapshot* active(std::uint8_t channel) const;
    const PlaybackProgramSnapshot* ready(std::uint8_t channel) const;
    bool transitionInFlight(std::uint8_t channel) const;
    std::uint32_t committedToken(std::uint8_t channel) const;
    PlaybackProgramExchangeStats stats() const;

private:
    struct ChannelSlot {
        ChannelSlot() noexcept
            : active(nullptr), ready(nullptr), retired(nullptr), committed_token(0), in_flight(false) {}

        std::atomic<const PlaybackProgramSnapshot*> active;
        std::atomic<const PlaybackProgramSnapshot*> ready;
        std::atomic<const PlaybackProgramSnapshot*> retired;
        std::atomic<std::uint32_t> committed_token;
        std::atomic<bool> in_flight;
    };

    bool validChannel(std::uint8_t channel) const { return channel < slots_.size(); }

    std::array<ChannelSlot, kMidiChannels> slots_{};
    std::atomic<std::uint64_t> ready_publishes_{0};
    std::atomic<std::uint64_t> ready_rejections_{0};
    std::atomic<std::uint64_t> commits_{0};
    std::atomic<std::uint64_t> commit_acks_{0};
};

// Audio-boundary bridge while residency and playback metadata remain separate
// ownership concepts. Control must publish matching READY snapshots to both
// exchanges. Under the existing single-control-owner contract, the preflight
// makes the two pointer commits a non-interleaved block-boundary transaction.
class ProgramRuntimeCommitCoordinator {
public:
    ProgramRuntimeCommitCoordinator(
        sample::ProgramSnapshotExchange& residency,
        PlaybackProgramSnapshotExchange& playback)
        : residency_(residency), playback_(playback) {}

    bool readyCompatible(std::uint8_t channel) const;
    bool activeCompatible(std::uint8_t channel) const;
    bool commitAtBlockBoundary(std::uint8_t channel);

private:
    static bool sameIdentity(
        const sample::ProgramSnapshot& residency,
        const PlaybackProgramSnapshot& playback);

    sample::ProgramSnapshotExchange& residency_;
    PlaybackProgramSnapshotExchange& playback_;
};

}  // namespace gm_synth::gm
