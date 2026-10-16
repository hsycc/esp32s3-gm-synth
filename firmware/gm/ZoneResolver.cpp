#include "ZoneResolver.h"

#include "sf2/Sf2FilterMath.h"
#include "sf2/Sf2ModulationMath.h"
#include "util/SaturatingCounter.h"

#include <algorithm>
#include <array>
#include <limits>

namespace gm_synth::gm {
namespace {

constexpr std::array<std::uint32_t, 12> kSemitoneRatioQ20{
    1048576u, 1110928u, 1176987u, 1246974u,
    1321123u, 1399681u, 1482910u, 1571089u,
    1664511u, 1763488u, 1868350u, 1979448u,
};

// 2^(cents/1200) in Q20 for cents=0..99. This keeps fractional Pitch Bend
// resolution out of floating point while retaining the existing semitone path.
constexpr std::array<std::uint32_t, 100> kCentRatioQ20{
    1048576u, 1049182u, 1049788u, 1050395u, 1051002u, 1051609u, 1052216u, 1052824u, 1053433u, 1054041u,
    1054650u, 1055260u, 1055869u, 1056479u, 1057090u, 1057701u, 1058312u, 1058923u, 1059535u, 1060147u,
    1060760u, 1061373u, 1061986u, 1062600u, 1063214u, 1063828u, 1064443u, 1065058u, 1065673u, 1066289u,
    1066905u, 1067521u, 1068138u, 1068755u, 1069373u, 1069991u, 1070609u, 1071227u, 1071846u, 1072466u,
    1073085u, 1073705u, 1074326u, 1074946u, 1075568u, 1076189u, 1076811u, 1077433u, 1078055u, 1078678u,
    1079302u, 1079925u, 1080549u, 1081174u, 1081798u, 1082423u, 1083049u, 1083674u, 1084301u, 1084927u,
    1085554u, 1086181u, 1086809u, 1087437u, 1088065u, 1088694u, 1089323u, 1089952u, 1090582u, 1091212u,
    1091843u, 1092473u, 1093105u, 1093736u, 1094368u, 1095000u, 1095633u, 1096266u, 1096900u, 1097533u,
    1098167u, 1098802u, 1099437u, 1100072u, 1100708u, 1101344u, 1101980u, 1102617u, 1103254u, 1103891u,
    1104529u, 1105167u, 1105806u, 1106445u, 1107084u, 1107724u, 1108364u, 1109004u, 1109645u, 1110286u,
};

constexpr std::uint32_t kRatioFractionBits = 20;
constexpr std::uint64_t kRatioRounding = std::uint64_t{1} << (kRatioFractionBits - 1u);

std::uint64_t multiplyRatioQ20(std::uint64_t value, std::uint32_t ratio_q20) {
    if (value == 0 || ratio_q20 == 0) {
        return 0;
    }
    const std::uint64_t max_product =
        std::numeric_limits<std::uint64_t>::max() - kRatioRounding;
    if (value > max_product / ratio_q20) {
        return std::numeric_limits<std::uint64_t>::max();
    }
    return (value * ratio_q20 + kRatioRounding) >> kRatioFractionBits;
}

bool validResolveArguments(
    const PlaybackProgramSnapshot& program,
    std::uint8_t channel,
    std::uint8_t key,
    std::uint8_t velocity,
    std::uint32_t note_instance_id,
    const ResolvedVoiceBatch* batch) {
    return batch != nullptr && channel < 16 && key <= 127 && velocity <= 127 &&
        note_instance_id != 0 && program.channel == channel &&
        program.zone_count <= PlaybackProgramSnapshot::kMaxZones &&
        (program.zone_count == 0 || program.zones != nullptr);
}

std::uint8_t effectiveVelocity(const PlaybackZone& zone, std::uint8_t velocity) {
    return zone.note_on_gain.enabled && zone.note_on_gain.fixed_velocity >= 0
        ? static_cast<std::uint8_t>(zone.note_on_gain.fixed_velocity)
        : velocity;
}

std::uint32_t scaleEnvelopeFramesByTimecents(
    std::uint32_t frames,
    int delta_timecents) {
    if (frames == 0 || delta_timecents == 0) {
        return frames;
    }
    const std::uint64_t base_q32 = static_cast<std::uint64_t>(frames) << 32u;
    const std::uint64_t scaled_q32 = ZoneResolver::transposeCents(
        base_q32,
        delta_timecents);
    const std::uint64_t whole_frames = scaled_q32 >> 32u;
    return whole_frames > std::numeric_limits<std::uint32_t>::max()
        ? std::numeric_limits<std::uint32_t>::max()
        : static_cast<std::uint32_t>(whole_frames);
}

void applyEnvelopeKeyScaling(
    const PlaybackZone& zone,
    std::uint8_t key,
    voice::EnvelopeConfig* envelope) {
    if (envelope == nullptr) {
        return;
    }
    const int key_delta = 60 - static_cast<int>(key);
    envelope->hold_frames = scaleEnvelopeFramesByTimecents(
        envelope->hold_frames,
        static_cast<int>(zone.envelope_key_scaling.hold_timecents_per_key) * key_delta);
    envelope->decay_frames = scaleEnvelopeFramesByTimecents(
        envelope->decay_frames,
        static_cast<int>(zone.envelope_key_scaling.decay_timecents_per_key) * key_delta);
}

void prepareNoteOnFilter(
    const PlaybackZone& zone,
    std::uint8_t effective_velocity,
    voice::BiquadLowpassConfig* output) {
    if (output == nullptr || !zone.note_on_filter.enabled) {
        return;
    }
    const std::int64_t summed_cutoff =
        static_cast<std::int64_t>(zone.note_on_filter.initial_cutoff_cents) +
        sf2::velocityToFilterCutoffCents(
            effective_velocity,
            zone.note_on_filter.velocity_to_cutoff_cents);
    const std::int32_t cutoff = static_cast<std::int32_t>(std::clamp<std::int64_t>(
        summed_cutoff,
        sf2::kSf2MinFilterCutoffCents,
        sf2::kSf2MaxFilterCutoffCents));

    // Preview zones with non-zero Q are already explicitly marked
    // PlaybackIssueFilterPending. Apply the correct cutoff with the qualified
    // Q=0 response rather than dropping filtering altogether.
    (void)sf2::prepareLowpassQ24(
        cutoff,
        0,
        sf2::kSf2FilterOutputSampleRate,
        output);
}

struct ActiveVoiceRefreshContext {
    const MidiChannelState* state = nullptr;
};

bool refreshActiveVoice(void* opaque, voice::Voice& voice) {
    auto* context = static_cast<ActiveVoiceRefreshContext*>(opaque);
    if (context == nullptr || context->state == nullptr) {
        return false;
    }

    const voice::VoiceRealtimeControlConfig& source = voice.realtimeControl();
    if (!source.enabled || source.unbent_phase_increment_q32 == 0) {
        return false;
    }

    voice::VoiceRuntimeControlUpdate update{};
    const int bend_cents = ZoneResolver::pitchBendCents(*context->state);
    update.phase_increment_q32 = bend_cents == 0
        ? source.unbent_phase_increment_q32
        : ZoneResolver::transposeCents(source.unbent_phase_increment_q32, bend_cents);
    update.gain_left_q15 = voice.gainLeftQ15();
    update.gain_right_q15 = voice.gainRightQ15();
    update.reverb_send_units = voice.reverbSendUnits();
    update.chorus_send_units = voice.chorusSendUnits();

    if (source.gain_enabled) {
        sf2::channelNoteOnGainQ15(
            source.initial_attenuation_cb,
            source.velocity_to_attenuation_cb,
            voice.velocity(),
            source.pan_sf2_units,
            context->state->volume,
            source.volume_to_attenuation_cb,
            context->state->pan,
            source.pan_amount,
            context->state->expression,
            source.expression_to_attenuation_cb,
            &update.gain_left_q15,
            &update.gain_right_q15);
    }

    if (source.effects_send_enabled) {
        update.reverb_send_units = static_cast<std::int16_t>(
            sf2::channelEffectsSendSf2Units(
                source.reverb_send_base_units,
                context->state->reverb_send,
                source.reverb_send_cc_amount));
        update.chorus_send_units = static_cast<std::int16_t>(
            sf2::channelEffectsSendSf2Units(
                source.chorus_send_base_units,
                context->state->chorus_send,
                source.chorus_send_cc_amount));
    }

    return voice.applyRuntimeControlUpdate(update);
}

}  // namespace

std::uint64_t ZoneResolver::transposeSemitones(
    std::uint64_t root_phase_increment_q32,
    int semitone_delta) {
    if (root_phase_increment_q32 == 0) {
        return 0;
    }

    int octaves = semitone_delta / 12;
    int remainder = semitone_delta % 12;
    if (remainder < 0) {
        remainder += 12;
        --octaves;
    }

    std::uint64_t value = multiplyRatioQ20(
        root_phase_increment_q32,
        kSemitoneRatioQ20[static_cast<std::size_t>(remainder)]);

    if (octaves > 0) {
        if (octaves >= 64 ||
            value > (std::numeric_limits<std::uint64_t>::max() >> octaves)) {
            return std::numeric_limits<std::uint64_t>::max();
        }
        value <<= octaves;
    } else if (octaves < 0) {
        const int shift = -octaves;
        if (shift >= 64) {
            return 1;
        }
        value >>= shift;
    }

    return std::max<std::uint64_t>(value, 1u);
}

std::uint64_t ZoneResolver::transposeCents(
    std::uint64_t root_phase_increment_q32,
    int cents_delta) {
    if (root_phase_increment_q32 == 0) {
        return 0;
    }

    int semitones = cents_delta / 100;
    int cents = cents_delta % 100;
    if (cents < 0) {
        cents += 100;
        --semitones;
    }

    const std::uint64_t semitone_value = transposeSemitones(
        root_phase_increment_q32,
        semitones);
    return std::max<std::uint64_t>(
        multiplyRatioQ20(
            semitone_value,
            kCentRatioQ20[static_cast<std::size_t>(cents)]),
        1u);
}

int ZoneResolver::pitchBendCents(const MidiChannelState& state) {
    const int delta = static_cast<int>(state.pitch_bend) -
        static_cast<int>(MidiChannelState::kPitchBendCenter);
    const int range_cents =
        static_cast<int>(state.pitch_bend_range_semitones) * 100 +
        static_cast<int>(state.pitch_bend_range_cents);
    const std::int64_t numerator = static_cast<std::int64_t>(delta) * range_cents;
    if (numerator >= 0) {
        return static_cast<int>((numerator + 4096) / 8192);
    }
    return -static_cast<int>((-numerator + 4096) / 8192);
}

ZoneResolveStatus ZoneResolver::resolveNoteOn(
    const PlaybackProgramSnapshot& program,
    std::uint8_t channel,
    std::uint8_t key,
    std::uint8_t velocity,
    std::uint32_t note_instance_id,
    ResolvedVoiceBatch* batch) const {
    if (!validResolveArguments(program, channel, key, velocity, note_instance_id, batch)) {
        return ZoneResolveStatus::InvalidArgument;
    }

    batch->clear();
    for (std::size_t i = 0; i < program.zone_count; ++i) {
        const PlaybackZone& zone = program.zones[i];
        if (!zone.matches(key, velocity)) {
            continue;
        }
        if (batch->count >= batch->voices.size()) {
            batch->clear();
            return ZoneResolveStatus::TooManyMatches;
        }

        const std::uint8_t effective_velocity = effectiveVelocity(zone, velocity);
        voice::VoiceStartParams params{};
        params.sample_id = zone.sample_id;
        params.channel = channel;
        params.key = key;
        params.velocity = effective_velocity;
        params.exclusive_class = zone.exclusive_class;
        params.note_instance_id = note_instance_id;
        params.phase_increment_q32 = zone.key_tracking
            ? transposeSemitones(
                  zone.root_phase_increment_q32,
                  static_cast<int>(key) - static_cast<int>(zone.root_key))
            : zone.root_phase_increment_q32;
        params.loop_mode = zone.loop_mode;
        params.sample_region = zone.sample_region;
        params.gain_left_q15 = zone.gain_left_q15;
        params.gain_right_q15 = zone.gain_right_q15;
        if (zone.note_on_gain.enabled) {
            sf2::noteOnGainQ15(
                zone.note_on_gain.initial_attenuation_cb,
                zone.note_on_gain.velocity_to_attenuation_cb,
                effective_velocity,
                zone.note_on_gain.pan_sf2_units,
                &params.gain_left_q15,
                &params.gain_right_q15);
        }
        if (zone.effects_send.enabled) {
            params.reverb_send_units = zone.effects_send.reverb_sf2_units;
            params.chorus_send_units = zone.effects_send.chorus_sf2_units;
        }
        params.envelope = zone.envelope;
        applyEnvelopeKeyScaling(zone, key, &params.envelope);
        prepareNoteOnFilter(zone, effective_velocity, &params.lowpass);
        batch->voices[batch->count++] = params;
    }

    return batch->count == 0 ? ZoneResolveStatus::NoMatch : ZoneResolveStatus::Ok;
}

ZoneResolveStatus ZoneResolver::resolveNoteOn(
    const PlaybackProgramSnapshot& program,
    const MidiChannelState& channel_state,
    std::uint8_t channel,
    std::uint8_t key,
    std::uint8_t velocity,
    std::uint32_t note_instance_id,
    ResolvedVoiceBatch* batch) const {
    if (!validResolveArguments(program, channel, key, velocity, note_instance_id, batch)) {
        return ZoneResolveStatus::InvalidArgument;
    }

    const int bend_cents = pitchBendCents(channel_state);
    batch->clear();
    for (std::size_t i = 0; i < program.zone_count; ++i) {
        const PlaybackZone& zone = program.zones[i];
        // Key/velocity ranges use the raw incoming NoteOn. Generator 46/47 and
        // controller modulators are applied only after the zone has matched.
        if (!zone.matches(key, velocity)) {
            continue;
        }
        if (batch->count >= batch->voices.size()) {
            batch->clear();
            return ZoneResolveStatus::TooManyMatches;
        }

        const std::uint8_t effective_velocity = effectiveVelocity(zone, velocity);
        voice::VoiceStartParams params{};
        params.sample_id = zone.sample_id;
        params.channel = channel;
        params.key = key;
        params.velocity = effective_velocity;
        params.exclusive_class = zone.exclusive_class;
        params.note_instance_id = note_instance_id;

        const std::uint64_t key_increment = zone.key_tracking
            ? transposeSemitones(
                  zone.root_phase_increment_q32,
                  static_cast<int>(key) - static_cast<int>(zone.root_key))
            : zone.root_phase_increment_q32;
        params.phase_increment_q32 = bend_cents == 0
            ? key_increment
            : transposeCents(key_increment, bend_cents);

        params.loop_mode = zone.loop_mode;
        params.sample_region = zone.sample_region;
        params.gain_left_q15 = zone.gain_left_q15;
        params.gain_right_q15 = zone.gain_right_q15;
        if (zone.note_on_gain.enabled) {
            sf2::channelNoteOnGainQ15(
                zone.note_on_gain.initial_attenuation_cb,
                zone.note_on_gain.velocity_to_attenuation_cb,
                effective_velocity,
                zone.note_on_gain.pan_sf2_units,
                channel_state.volume,
                zone.note_on_gain.volume_to_attenuation_cb,
                channel_state.pan,
                zone.note_on_gain.pan_amount,
                channel_state.expression,
                zone.note_on_gain.expression_to_attenuation_cb,
                &params.gain_left_q15,
                &params.gain_right_q15);
        }
        if (zone.effects_send.enabled) {
            params.reverb_send_units = static_cast<std::int16_t>(
                sf2::channelEffectsSendSf2Units(
                    zone.effects_send.reverb_sf2_units,
                    channel_state.reverb_send,
                    zone.effects_send.reverb_cc_amount));
            params.chorus_send_units = static_cast<std::int16_t>(
                sf2::channelEffectsSendSf2Units(
                    zone.effects_send.chorus_sf2_units,
                    channel_state.chorus_send,
                    zone.effects_send.chorus_cc_amount));
        }
        params.envelope = zone.envelope;
        applyEnvelopeKeyScaling(zone, key, &params.envelope);
        prepareNoteOnFilter(zone, effective_velocity, &params.lowpass);
        params.realtime_control.enabled = true;
        params.realtime_control.gain_enabled = zone.note_on_gain.enabled;
        params.realtime_control.effects_send_enabled = zone.effects_send.enabled;
        params.realtime_control.unbent_phase_increment_q32 = key_increment;
        params.realtime_control.initial_attenuation_cb =
            zone.note_on_gain.initial_attenuation_cb;
        params.realtime_control.velocity_to_attenuation_cb =
            zone.note_on_gain.velocity_to_attenuation_cb;
        params.realtime_control.volume_to_attenuation_cb =
            zone.note_on_gain.volume_to_attenuation_cb;
        params.realtime_control.pan_amount = zone.note_on_gain.pan_amount;
        params.realtime_control.expression_to_attenuation_cb =
            zone.note_on_gain.expression_to_attenuation_cb;
        params.realtime_control.pan_sf2_units = zone.note_on_gain.pan_sf2_units;
        params.realtime_control.reverb_send_base_units =
            zone.effects_send.reverb_sf2_units;
        params.realtime_control.chorus_send_base_units =
            zone.effects_send.chorus_sf2_units;
        params.realtime_control.reverb_send_cc_amount =
            zone.effects_send.reverb_cc_amount;
        params.realtime_control.chorus_send_cc_amount =
            zone.effects_send.chorus_cc_amount;
        batch->voices[batch->count++] = params;
    }

    return batch->count == 0 ? ZoneResolveStatus::NoMatch : ZoneResolveStatus::Ok;
}

std::uint32_t RealtimeProgramEventApplier::allocateNoteInstanceId() {
    const std::uint32_t id = next_note_instance_id_;
    ++next_note_instance_id_;
    if (next_note_instance_id_ == 0) {
        next_note_instance_id_ = 1;
    }
    return id;
}

std::size_t RealtimeProgramEventApplier::refreshActiveVoices(
    std::uint8_t channel,
    voice::VoicePool& voices) {
    ActiveVoiceRefreshContext context{&channel_states_.channel(channel)};
    const std::size_t updated = voices.updateActiveVoices(
        channel,
        &refreshActiveVoice,
        &context);
    util::saturatingAdd(stats_.active_voice_updates, updated);
    return updated;
}

bool RealtimeProgramEventApplier::apply(
    const realtime::RealtimeEvent& event,
    voice::VoicePool& voices) {
    if (!event.valid() || event.data1 > 127 || event.data2 > 127) {
        util::saturatingIncrement(stats_.invalid_events);
        return false;
    }

    if (event.type == realtime::EventType::NoteOff ||
        (event.type == realtime::EventType::NoteOn && event.data2 == 0)) {
        util::saturatingIncrement(stats_.note_off_events);
        const MidiChannelState& state = channel_states_.channel(event.channel);
        voices.noteOff(event.channel, event.data1, state.sustainDown());
        return true;
    }

    if (event.type == realtime::EventType::ControlChange) {
        util::saturatingIncrement(stats_.control_change_events);
        const bool sustain_before = channel_states_.channel(event.channel).sustainDown();
        const std::uint32_t action = channel_states_.applyControlChange(
            event.channel,
            event.data1,
            event.data2);
        const MidiChannelState& state = channel_states_.channel(event.channel);
        if (action != ChannelControlNone) {
            util::saturatingIncrement(stats_.controller_state_changes);
        }

        if ((action & ChannelControlAllSoundOff) != 0) {
            util::saturatingIncrement(stats_.all_sound_off_events);
            voices.allSoundOff(event.channel);
        }
        if ((action & ChannelControlAllNotesOff) != 0) {
            util::saturatingIncrement(stats_.all_notes_off_events);
            voices.allNotesOff(event.channel, state.sustainDown());
        }
        if ((action & ChannelControlSustainChanged) != 0 &&
            sustain_before && !state.sustainDown()) {
            const std::size_t released = voices.releaseSustain(event.channel);
            util::saturatingAdd(stats_.sustain_releases, released);
        }

        constexpr std::uint32_t kRefreshMask =
            ChannelControlGainChanged |
            ChannelControlPanChanged |
            ChannelControlEffectsChanged |
            ChannelControlPitchRangeChanged |
            ChannelControlResetControllers;
        if ((action & kRefreshMask) != 0 &&
            (action & ChannelControlAllSoundOff) == 0) {
            refreshActiveVoices(event.channel, voices);
        }
        return true;
    }

    if (event.type == realtime::EventType::PitchBend) {
        util::saturatingIncrement(stats_.pitch_bend_events);
        if (!channel_states_.applyPitchBend(event.channel, event.value)) {
            util::saturatingIncrement(stats_.invalid_events);
            return false;
        }
        util::saturatingIncrement(stats_.controller_state_changes);
        refreshActiveVoices(event.channel, voices);
        return true;
    }

    if (event.type != realtime::EventType::NoteOn) {
        util::saturatingIncrement(stats_.unsupported_events);
        return false;
    }

    util::saturatingIncrement(stats_.note_on_events);
    const PlaybackProgramSnapshot* program = programs_.active(event.channel);
    if (program == nullptr) {
        util::saturatingIncrement(stats_.missing_active_program);
        return false;
    }

    // Never use an old melodic fallback for GM Channel 10. Program Prepare may
    // be pending, or the loaded SF2 may have no bank-128 drum preset at all; in
    // either case percussion NoteOn remains silent until a real drum snapshot
    // is active instead of producing the wrong instrument.
    if (event.channel == kGmPercussionChannel && program->bank != kSf2PercussionBank) {
        util::saturatingIncrement(stats_.percussion_program_unavailable);
        return true;
    }

    const ZoneResolveStatus status = resolver_.resolveNoteOn(
        *program,
        channel_states_.channel(event.channel),
        event.channel,
        event.data1,
        event.data2,
        allocateNoteInstanceId(),
        &batch_);

    if (status == ZoneResolveStatus::NoMatch) {
        util::saturatingIncrement(stats_.no_matching_zones);
        return true;
    }
    if (status == ZoneResolveStatus::TooManyMatches) {
        util::saturatingIncrement(stats_.resolve_overflows);
        return false;
    }
    if (status != ZoneResolveStatus::Ok) {
        util::saturatingIncrement(stats_.invalid_events);
        return false;
    }

    util::saturatingAdd(stats_.voices_resolved, static_cast<std::uint64_t>(batch_.count));
    bool all_started = true;
    for (std::size_t i = 0; i < batch_.count; ++i) {
        if (voices.start(batch_.voices[i])) {
            util::saturatingIncrement(stats_.voices_started);
        } else {
            util::saturatingIncrement(stats_.voice_start_failures);
            all_started = false;
        }
    }
    return all_started;
}

bool RealtimeProgramEventApplier::callback(
    void* context,
    const realtime::RealtimeEvent& event,
    voice::VoicePool& voices) {
    if (context == nullptr) {
        return false;
    }
    return static_cast<RealtimeProgramEventApplier*>(context)->apply(event, voices);
}

}  // namespace gm_synth::gm
