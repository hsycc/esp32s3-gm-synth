#include "gm/MidiChannelState.h"
#include "gm/PlaybackProgramSnapshot.h"
#include "gm/ZoneResolver.h"
#include "sample/PublishedSampleDirectory.h"
#include "sf2/Sf2ModulationMath.h"
#include "sf2/Sf2PanMath.h"
#include "voice/VoiceRenderer.h"

#include <array>
#include <cassert>
#include <cstdint>
#include <iostream>

namespace {

using gm_synth::gm::MidiChannelState;
using gm_synth::gm::PlaybackProgramSnapshot;
using gm_synth::gm::PlaybackProgramSnapshotExchange;
using gm_synth::gm::PlaybackZone;
using gm_synth::gm::RealtimeProgramEventApplier;
using gm_synth::gm::ResolvedVoiceBatch;
using gm_synth::gm::ZoneResolveStatus;
using gm_synth::gm::ZoneResolver;
using gm_synth::realtime::EventType;
using gm_synth::realtime::RealtimeEvent;
using gm_synth::sample::PublishedSampleDirectory;
using gm_synth::sf2::attenuationCbToGainQ15;
using gm_synth::sf2::controllerToAttenuationCb;
using gm_synth::sf2::sf2PanChannelGainQ15;
using gm_synth::voice::VoicePool;
using gm_synth::voice::kPhaseOneQ32;
using gm_synth::voice::kQ15Unity;

std::int32_t multiplyQ15(std::int32_t a, std::int32_t b) {
    return static_cast<std::int32_t>(
        (static_cast<std::int64_t>(a) * b + (std::int64_t{1} << 14)) >> 15);
}

PlaybackZone controllerZone() {
    PlaybackZone zone{};
    zone.sample_id = 1;
    zone.key_low = 0;
    zone.key_high = 127;
    zone.velocity_low = 1;
    zone.velocity_high = 127;
    zone.root_key = 60;
    zone.key_tracking = false;
    zone.root_phase_increment_q32 = kPhaseOneQ32;
    zone.note_on_gain.enabled = true;
    zone.note_on_gain.initial_attenuation_cb = 0;
    zone.note_on_gain.velocity_to_attenuation_cb = 0;
    zone.note_on_gain.volume_to_attenuation_cb = 960;
    zone.note_on_gain.pan_amount = 500;
    zone.note_on_gain.expression_to_attenuation_cb = 960;
    zone.note_on_gain.pan_sf2_units = 0;
    zone.note_on_gain.fixed_velocity = -1;
    return zone;
}

PlaybackProgramSnapshot programFor(const PlaybackZone* zone) {
    PlaybackProgramSnapshot program{};
    program.channel = 2;
    program.bank = 0;
    program.program = 0;
    program.token = 1;
    program.bank_generation = 1;
    program.zones = zone;
    program.zone_count = 1;
    return program;
}

void testDefaultGmVolumeIsAppliedOnlyOnChannelStatePath() {
    const PlaybackZone zone = controllerZone();
    const PlaybackProgramSnapshot program = programFor(&zone);
    assert(program.valid());

    ZoneResolver resolver;
    ResolvedVoiceBatch legacy{};
    ResolvedVoiceBatch controlled{};
    MidiChannelState state{};
    state.resetAll();

    assert(resolver.resolveNoteOn(program, 2, 60, 127, 1, &legacy) == ZoneResolveStatus::Ok);
    assert(resolver.resolveNoteOn(program, state, 2, 60, 127, 2, &controlled) ==
           ZoneResolveStatus::Ok);
    assert(legacy.count == 1 && controlled.count == 1);
    assert(legacy.voices[0].gain_left_q15 == sf2PanChannelGainQ15(0, true));
    assert(legacy.voices[0].gain_right_q15 == sf2PanChannelGainQ15(0, false));

    const std::int32_t expected_attenuation = controllerToAttenuationCb(100, 960);
    const std::int32_t expected_amplitude = attenuationCbToGainQ15(expected_attenuation);
    const std::int32_t expected_gain = multiplyQ15(
        expected_amplitude,
        sf2PanChannelGainQ15(0, true));
    assert(expected_attenuation > 0);
    assert(controlled.voices[0].gain_left_q15 == expected_gain);
    assert(controlled.voices[0].gain_right_q15 == expected_gain);
    assert(expected_gain < sf2PanChannelGainQ15(0, true));
}

void testCc7Cc11AndCc10AffectSubsequentNoteOn() {
    const PlaybackZone zone = controllerZone();
    const PlaybackProgramSnapshot program = programFor(&zone);
    ZoneResolver resolver;
    ResolvedVoiceBatch batch{};
    MidiChannelState state{};
    state.resetAll();

    state.applyControlChange(7, 127);
    state.applyControlChange(11, 127);
    assert(resolver.resolveNoteOn(program, state, 2, 60, 100, 3, &batch) ==
           ZoneResolveStatus::Ok);
    assert(batch.voices[0].gain_left_q15 == sf2PanChannelGainQ15(0, true));
    assert(batch.voices[0].gain_right_q15 == sf2PanChannelGainQ15(0, false));

    state.applyControlChange(7, 80);
    state.applyControlChange(11, 100);
    assert(resolver.resolveNoteOn(program, state, 2, 60, 100, 4, &batch) ==
           ZoneResolveStatus::Ok);
    const std::int32_t attenuation =
        controllerToAttenuationCb(80, 960) +
        controllerToAttenuationCb(100, 960);
    const std::int32_t amplitude = attenuationCbToGainQ15(attenuation);
    const std::int32_t centered = multiplyQ15(amplitude, sf2PanChannelGainQ15(0, true));
    assert(batch.voices[0].gain_left_q15 == centered);
    assert(batch.voices[0].gain_right_q15 == centered);

    state.applyControlChange(7, 127);
    state.applyControlChange(11, 127);
    state.applyControlChange(10, 0);
    assert(resolver.resolveNoteOn(program, state, 2, 60, 100, 5, &batch) ==
           ZoneResolveStatus::Ok);
    assert(batch.voices[0].gain_left_q15 == kQ15Unity);
    assert(batch.voices[0].gain_right_q15 == 0);

    state.applyControlChange(10, 127);
    assert(resolver.resolveNoteOn(program, state, 2, 60, 100, 6, &batch) ==
           ZoneResolveStatus::Ok);
    assert(batch.voices[0].gain_left_q15 == sf2PanChannelGainQ15(492, true));
    assert(batch.voices[0].gain_right_q15 == sf2PanChannelGainQ15(492, false));
    assert(batch.voices[0].gain_right_q15 > 32700);
    assert(batch.voices[0].gain_left_q15 > 0);
}

void testPitchBendUsesRpnSensitivityWithoutFloatingPoint() {
    const PlaybackZone zone = controllerZone();
    const PlaybackProgramSnapshot program = programFor(&zone);
    ZoneResolver resolver;
    ResolvedVoiceBatch batch{};
    MidiChannelState state{};
    state.resetAll();
    state.applyControlChange(7, 127);

    assert(ZoneResolver::pitchBendCents(state) == 0);
    assert(state.applyPitchBend(16383));
    assert(ZoneResolver::pitchBendCents(state) == 200);
    assert(resolver.resolveNoteOn(program, state, 2, 60, 100, 7, &batch) ==
           ZoneResolveStatus::Ok);
    assert(batch.voices[0].phase_increment_q32 ==
           ZoneResolver::transposeSemitones(kPhaseOneQ32, 2));

    assert(state.applyPitchBend(0));
    assert(ZoneResolver::pitchBendCents(state) == -200);
    assert(resolver.resolveNoteOn(program, state, 2, 60, 100, 8, &batch) ==
           ZoneResolveStatus::Ok);
    assert(batch.voices[0].phase_increment_q32 ==
           ZoneResolver::transposeSemitones(kPhaseOneQ32, -2));

    state.applyControlChange(101, 0);
    state.applyControlChange(100, 0);
    state.applyControlChange(6, 12);
    state.applyControlChange(38, 50);
    assert(state.applyPitchBend(16383));
    assert(ZoneResolver::pitchBendCents(state) == 1250);
    assert(resolver.resolveNoteOn(program, state, 2, 60, 100, 9, &batch) ==
           ZoneResolveStatus::Ok);
    assert(batch.voices[0].phase_increment_q32 ==
           ZoneResolver::transposeCents(kPhaseOneQ32, 1250));
}

void testRealtimeApplierOwnsSampleAccurateChannelState() {
    PlaybackProgramSnapshotExchange programs;
    RealtimeProgramEventApplier applier(programs);
    PublishedSampleDirectory directory;
    VoicePool voices(directory);

    RealtimeEvent cc{};
    cc.type = EventType::ControlChange;
    cc.channel = 2;
    cc.data1 = 7;
    cc.data2 = 127;
    assert(applier.apply(cc, voices));
    assert(applier.channelState(2).volume == 127);

    cc.data1 = 101;
    cc.data2 = 0;
    assert(applier.apply(cc, voices));
    cc.data1 = 100;
    cc.data2 = 0;
    assert(applier.apply(cc, voices));
    cc.data1 = 6;
    cc.data2 = 12;
    assert(applier.apply(cc, voices));
    assert(applier.channelState(2).pitch_bend_range_semitones == 12);

    RealtimeEvent bend{};
    bend.type = EventType::PitchBend;
    bend.channel = 2;
    bend.value = 16383;
    assert(applier.apply(bend, voices));
    assert(applier.channelState(2).pitch_bend == 16383);

    bend.value = 16384;
    assert(!applier.apply(bend, voices));
    assert(applier.channelState(2).pitch_bend == 16383);

    const auto stats = applier.stats();
    assert(stats.control_change_events == 4);
    assert(stats.pitch_bend_events == 2);
    assert(stats.controller_state_changes == 5);
    assert(stats.invalid_events == 1);
}

}  // namespace

int main() {
    testDefaultGmVolumeIsAppliedOnlyOnChannelStatePath();
    testCc7Cc11AndCc10AffectSubsequentNoteOn();
    testPitchBendUsesRpnSensitivityWithoutFloatingPoint();
    testRealtimeApplierOwnsSampleAccurateChannelState();
    std::cout << "GM channel-state NoteOn runtime checks passed\n";
    return 0;
}