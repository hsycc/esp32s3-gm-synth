#include "../../firmware/gm/PlaybackProgramSnapshot.h"
#include "../../firmware/gm/ZoneResolver.h"
#include "../../firmware/sf2/Sf2ModulationMath.h"
#include "../../firmware/sf2/Sf2PanMath.h"
#include "../../firmware/sf2/Sf2PlaybackMaterializer.h"

#include <algorithm>
#include <array>
#include <cassert>
#include <cstdint>
#include <iostream>

namespace {

using gm_synth::gm::PlaybackProgramSnapshot;
using gm_synth::gm::PlaybackZone;
using gm_synth::gm::ResolvedVoiceBatch;
using gm_synth::gm::ZoneResolveStatus;
using gm_synth::gm::ZoneResolver;
using gm_synth::sf2::NativeResolvedZone;
using gm_synth::sf2::PlaybackIssueCustomModulatorsPending;
using gm_synth::sf2::PlaybackIssueFixedVelocityPending;
using gm_synth::sf2::ResolvedZoneModulation;
using gm_synth::sf2::Sf2PlaybackMaterializer;
using gm_synth::sf2::attenuationCbToGainQ15;
using gm_synth::sf2::sf2PanChannelGainQ15;
using gm_synth::sf2::velocityToAttenuationCb;
using gm_synth::voice::kPhaseOneQ32;
using gm_synth::voice::kQ15Unity;

std::int32_t multiplyQ15(std::int32_t a, std::int32_t b) {
    return static_cast<std::int32_t>(
        (static_cast<std::int64_t>(a) * b + (std::int64_t{1} << 14)) >> 15);
}

NativeResolvedZone baseNativeZone() {
    NativeResolvedZone zone{};
    zone.bank = 0;
    zone.program = 0;
    zone.sample_id = 1;
    zone.key_low = 0;
    zone.key_high = 127;
    zone.velocity_low = 1;
    zone.velocity_high = 127;
    zone.sample_start = 100;
    zone.sample_end = 200;
    zone.loop_start = 120;
    zone.loop_end = 180;
    zone.shdr_start = 100;
    zone.shdr_end = 200;
    zone.shdr_loop_start = 120;
    zone.shdr_loop_end = 180;
    zone.sample_rate = 44100;
    zone.sample_root_key = 60;
    zone.sample_type = 1;
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

void testMaterializerCarriesResolvedVelocityRouteAndFixedVelocity() {
    NativeResolvedZone native = baseNativeZone();
    native.generators.set(47, 64);
    native.generators.set(48, -100);

    ResolvedZoneModulation modulation{};
    modulation.velocity_to_attenuation_cb = 960;
    modulation.supported_velocity_attenuation_records = 1;

    PlaybackZone zone{};
    const auto result = Sf2PlaybackMaterializer{}.materialize(native, modulation, &zone);
    assert(result.playablePreview());
    assert(!result.hasIssue(PlaybackIssueFixedVelocityPending));
    assert(!result.hasIssue(PlaybackIssueCustomModulatorsPending));
    assert(zone.note_on_gain.enabled);
    assert(zone.note_on_gain.initial_attenuation_cb == -100);
    assert(zone.note_on_gain.velocity_to_attenuation_cb == 960);
    assert(zone.note_on_gain.fixed_velocity == 64);
    assert(zone.valid());

    modulation.unsupported_modulator_records = 1;
    const auto unsupported = Sf2PlaybackMaterializer{}.materialize(native, modulation, &zone);
    assert(unsupported.playablePreview());
    assert(unsupported.hasIssue(PlaybackIssueCustomModulatorsPending));
}

void testResolverAppliesVelocityAttenuationAtNoteOn() {
    PlaybackZone zone{};
    zone.sample_id = 1;
    zone.key_low = 0;
    zone.key_high = 127;
    zone.velocity_low = 1;
    zone.velocity_high = 127;
    zone.root_key = 60;
    zone.root_phase_increment_q32 = kPhaseOneQ32;
    zone.gain_left_q15 = 1234;
    zone.gain_right_q15 = 2345;
    zone.note_on_gain.enabled = true;
    zone.note_on_gain.velocity_to_attenuation_cb = 960;

    const PlaybackProgramSnapshot program = programFor(&zone);
    assert(program.valid());

    ZoneResolver resolver;
    ResolvedVoiceBatch batch{};
    assert(resolver.resolveNoteOn(program, 2, 60, 127, 10, &batch) == ZoneResolveStatus::Ok);
    assert(batch.count == 1);
    assert(batch.voices[0].velocity == 127);
    assert(batch.voices[0].gain_left_q15 == sf2PanChannelGainQ15(0, true));
    assert(batch.voices[0].gain_right_q15 == sf2PanChannelGainQ15(0, false));

    assert(resolver.resolveNoteOn(program, 2, 60, 111, 11, &batch) == ZoneResolveStatus::Ok);
    const std::int32_t attenuation = velocityToAttenuationCb(111, 960);
    const std::int32_t expected_amplitude = attenuationCbToGainQ15(attenuation);
    const std::int32_t expected_gain = multiplyQ15(
        expected_amplitude,
        sf2PanChannelGainQ15(0, true));
    assert(attenuation == 23);
    assert(batch.voices[0].gain_left_q15 == expected_gain);
    assert(batch.voices[0].gain_right_q15 == expected_gain);
    assert(expected_gain < kQ15Unity);
}

void testFixedVelocityIsAppliedAfterZoneSelection() {
    PlaybackZone zone{};
    zone.sample_id = 1;
    zone.key_low = 0;
    zone.key_high = 127;
    zone.velocity_low = 100;
    zone.velocity_high = 127;
    zone.root_key = 60;
    zone.root_phase_increment_q32 = kPhaseOneQ32;
    zone.note_on_gain.enabled = true;
    zone.note_on_gain.initial_attenuation_cb = -100;
    zone.note_on_gain.velocity_to_attenuation_cb = 960;
    zone.note_on_gain.fixed_velocity = 64;

    const PlaybackProgramSnapshot program = programFor(&zone);
    assert(program.valid());

    ZoneResolver resolver;
    ResolvedVoiceBatch batch{};
    assert(resolver.resolveNoteOn(program, 2, 60, 111, 12, &batch) == ZoneResolveStatus::Ok);
    assert(batch.count == 1);
    assert(batch.voices[0].velocity == 64);
    const std::int32_t expected_attenuation =
        std::max(0, -100 + velocityToAttenuationCb(64, 960));
    const std::int32_t expected_amplitude = attenuationCbToGainQ15(expected_attenuation);
    const std::int32_t expected_gain = multiplyQ15(
        expected_amplitude,
        sf2PanChannelGainQ15(0, true));
    assert(batch.voices[0].gain_left_q15 == expected_gain);
    assert(batch.voices[0].gain_right_q15 == expected_gain);

    assert(resolver.resolveNoteOn(program, 2, 60, 70, 13, &batch) == ZoneResolveStatus::NoMatch);
    assert(batch.count == 0);
}

void testLegacyPlaybackZoneKeepsPreparedStaticGain() {
    PlaybackZone zone{};
    zone.sample_id = 1;
    zone.root_key = 60;
    zone.root_phase_increment_q32 = kPhaseOneQ32;
    zone.gain_left_q15 = 12345;
    zone.gain_right_q15 = 23456;
    assert(!zone.note_on_gain.enabled);

    const PlaybackProgramSnapshot program = programFor(&zone);
    ZoneResolver resolver;
    ResolvedVoiceBatch batch{};
    assert(resolver.resolveNoteOn(program, 2, 60, 90, 14, &batch) == ZoneResolveStatus::Ok);
    assert(batch.voices[0].gain_left_q15 == 12345);
    assert(batch.voices[0].gain_right_q15 == 23456);
}

}  // namespace

int main() {
    testMaterializerCarriesResolvedVelocityRouteAndFixedVelocity();
    testResolverAppliesVelocityAttenuationAtNoteOn();
    testFixedVelocityIsAppliedAfterZoneSelection();
    testLegacyPlaybackZoneKeepsPreparedStaticGain();
    std::cout << "SF2 note-on modulation runtime checks passed\n";
    return 0;
}