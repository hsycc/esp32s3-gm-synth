#include "../../firmware/gm/MidiChannelState.h"
#include "../../firmware/gm/PlaybackProgramSnapshot.h"
#include "../../firmware/gm/ZoneResolver.h"
#include "../../firmware/sf2/Sf2ModulationMath.h"
#include "../../firmware/sf2/Sf2PanMath.h"
#include "../../firmware/voice/VoiceRenderer.h"

#include <cassert>
#include <cstdint>
#include <iostream>

namespace {

using gm_synth::gm::MidiChannelState;
using gm_synth::gm::PlaybackProgramSnapshot;
using gm_synth::gm::PlaybackZone;
using gm_synth::gm::ResolvedVoiceBatch;
using gm_synth::gm::ZoneResolveStatus;
using gm_synth::gm::ZoneResolver;
using gm_synth::sf2::channelNoteOnGainQ15;
using gm_synth::sf2::controllerToPanSf2Units;
using gm_synth::sf2::sf2PanChannelGainQ15;
using gm_synth::voice::kPhaseOneQ32;
using gm_synth::voice::kQ15Unity;

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

PlaybackZone tunedZone(std::uint16_t cents_per_key) {
    PlaybackZone zone{};
    zone.sample_id = 0;
    zone.root_key = 60;
    zone.key_tracking = cents_per_key != 0;
    zone.scale_tuning_cents_per_key = cents_per_key;
    zone.root_phase_increment_q32 = kPhaseOneQ32;
    zone.note_on_gain.enabled = true;
    zone.note_on_gain.velocity_to_attenuation_cb = 0;
    zone.note_on_gain.volume_to_attenuation_cb = 0;
    zone.note_on_gain.expression_to_attenuation_cb = 0;
    zone.note_on_gain.pan_amount = 500;
    return zone;
}

void testFiftyCentsPerKeyIsAppliedOnBothResolverPaths() {
    PlaybackZone zone = tunedZone(50);
    const PlaybackProgramSnapshot program = programFor(&zone);
    assert(program.valid());

    ZoneResolver resolver;
    ResolvedVoiceBatch legacy{};
    assert(resolver.resolveNoteOn(program, 2, 72, 100, 1, &legacy) == ZoneResolveStatus::Ok);
    assert(legacy.count == 1);
    assert(legacy.voices[0].phase_increment_q32 ==
           ZoneResolver::transposeCents(kPhaseOneQ32, 600));

    MidiChannelState state{};
    state.resetAll();
    state.pitch_bend = 16383;
    ResolvedVoiceBatch controlled{};
    assert(resolver.resolveNoteOn(program, state, 2, 72, 100, 2, &controlled) ==
           ZoneResolveStatus::Ok);
    assert(controlled.voices[0].phase_increment_q32 ==
           ZoneResolver::transposeCents(kPhaseOneQ32, 800));
    assert(controlled.voices[0].realtime_control.unbent_phase_increment_q32 ==
           ZoneResolver::transposeCents(kPhaseOneQ32, 600));
}

void testFullScaleTuningRangeRemainsRepresentable() {
    PlaybackZone octave_per_key = tunedZone(1200);
    const PlaybackProgramSnapshot program = programFor(&octave_per_key);
    ZoneResolver resolver;
    ResolvedVoiceBatch batch{};
    assert(resolver.resolveNoteOn(program, 2, 61, 100, 3, &batch) == ZoneResolveStatus::Ok);
    assert(batch.voices[0].phase_increment_q32 ==
           ZoneResolver::transposeCents(kPhaseOneQ32, 1200));

    PlaybackZone fixed = tunedZone(0);
    assert(!fixed.key_tracking);
    const PlaybackProgramSnapshot fixed_program = programFor(&fixed);
    assert(resolver.resolveNoteOn(fixed_program, 2, 96, 100, 4, &batch) ==
           ZoneResolveStatus::Ok);
    assert(batch.voices[0].phase_increment_q32 == kPhaseOneQ32);
}

void testDefaultCc10AndEqualPowerPanAreConsistent() {
    assert(controllerToPanSf2Units(0) == -500);
    assert(controllerToPanSf2Units(64) == 0);
    assert(controllerToPanSf2Units(127) == 492);
    assert(sf2PanChannelGainQ15(0, true) == 23170);
    assert(sf2PanChannelGainQ15(0, false) == 23170);

    std::int32_t left = 0;
    std::int32_t right = 0;
    channelNoteOnGainQ15(
        0, 0, 100, 0,
        127, 0,
        64, 500,
        127, 0,
        &left, &right);
    assert(left == 23170);
    assert(right == 23170);

    channelNoteOnGainQ15(
        0, 0, 100, 0,
        127, 0,
        0, 500,
        127, 0,
        &left, &right);
    assert(left == kQ15Unity);
    assert(right == 0);

    channelNoteOnGainQ15(
        0, 0, 100, 0,
        127, 0,
        127, 500,
        127, 0,
        &left, &right);
    assert(left == sf2PanChannelGainQ15(492, true));
    assert(right == sf2PanChannelGainQ15(492, false));
}

}  // namespace

int main() {
    testFiftyCentsPerKeyIsAppliedOnBothResolverPaths();
    testFullScaleTuningRangeRemainsRepresentable();
    testDefaultCc10AndEqualPowerPanAreConsistent();
    std::cout << "SF2 scale-tuning/pan runtime checks passed\n";
    return 0;
}