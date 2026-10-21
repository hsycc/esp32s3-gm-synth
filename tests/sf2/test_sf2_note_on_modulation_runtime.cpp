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
using gm_synth::sf2::Sf2NoteOnSourceRoute;
using gm_synth::sf2::Sf2PlaybackMaterializer;
using gm_synth::sf2::Sf2PreparedNoteOnModulator;
using gm_synth::sf2::addPreparedNoteOnModulator;
using gm_synth::sf2::attenuationCbToGainQ15;
using gm_synth::sf2::sf2PanChannelGainQ15;
using gm_synth::sf2::velocityToAttenuationCb;
using gm_synth::voice::BiquadLowpassConfig;
using gm_synth::voice::kPhaseOneQ32;
using gm_synth::voice::kQ15Unity;

std::int32_t multiplyQ15(std::int32_t a, std::int32_t b) {
    return static_cast<std::int32_t>(
        (static_cast<std::int64_t>(a) * b + (std::int64_t{1} << 14)) >> 15);
}

bool sameFilter(const BiquadLowpassConfig& a, const BiquadLowpassConfig& b) {
    return a.enabled == b.enabled &&
        a.a0_q24 == b.a0_q24 &&
        a.a1_q24 == b.a1_q24 &&
        a.b1_q24 == b.b1_q24 &&
        a.b2_q24 == b.b2_q24;
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

void testGenericNoteOnFilterSourcesMaterializeAndRun() {
    NativeResolvedZone native = baseNativeZone();
    native.generators.set(8, 8000);  // initialFilterFc

    ResolvedZoneModulation modulation{};
    // Isolate the generic routes from the mandatory default velocity route.
    modulation.velocity_to_filter_fc_cents = 0;
    modulation.initial_filter_fc_note_on_routes[0] =
        Sf2NoteOnSourceRoute{0x0203u, 1200};  // key, +bipolar linear
    modulation.initial_filter_fc_note_on_routes[1] =
        Sf2NoteOnSourceRoute{0x0902u, 1200};  // velocity, -unipolar convex
    modulation.initial_filter_fc_note_on_route_count = 2;
    modulation.supported_note_on_source_records = 2;

    PlaybackZone zone{};
    const auto materialized = Sf2PlaybackMaterializer{}.materialize(native, modulation, &zone);
    assert(materialized.playablePreview());
    assert(!materialized.hasIssue(PlaybackIssueCustomModulatorsPending));
    assert(zone.note_on_filter.enabled);
    assert(zone.note_on_filter.initial_cutoff_cents == 8000);
    assert(zone.note_on_filter.additional_cutoff_route_count == 2);
    assert(zone.note_on_filter.additional_cutoff_routes[0].source_oper == 0x0203u);
    assert(zone.note_on_filter.additional_cutoff_routes[1].source_oper == 0x0902u);
    assert(zone.valid());

    const PlaybackProgramSnapshot program = programFor(&zone);
    ZoneResolver resolver;
    ResolvedVoiceBatch batch{};

    // key64 is the bipolar-linear zero point and velocity127 is the zero point
    // of the negative convex route, so this is the 8000-cent baseline.
    assert(resolver.resolveNoteOn(program, 2, 64, 127, 20, &batch) == ZoneResolveStatus::Ok);
    const BiquadLowpassConfig baseline = batch.voices[0].lowpass;
    assert(baseline.enabled);

    // Key movement changes only the 0x0203 route here.
    assert(resolver.resolveNoteOn(program, 2, 96, 127, 21, &batch) == ZoneResolveStatus::Ok);
    const BiquadLowpassConfig key_shifted = batch.voices[0].lowpass;
    assert(key_shifted.enabled);
    assert(!sameFilter(key_shifted, baseline));
    assert(key_shifted.a0_q24 > baseline.a0_q24);

    // Velocity movement changes only the 0x0902 convex route at key64.
    assert(resolver.resolveNoteOn(program, 2, 64, 64, 22, &batch) == ZoneResolveStatus::Ok);
    const BiquadLowpassConfig velocity_shifted = batch.voices[0].lowpass;
    assert(velocity_shifted.enabled);
    assert(!sameFilter(velocity_shifted, baseline));
    assert(velocity_shifted.a0_q24 > baseline.a0_q24);

    // Generator 47 replaces the velocity used by velocity modulators after
    // zone selection. It must not replace the actual NoteOn key source.
    PlaybackZone fixed_velocity_zone = zone;
    fixed_velocity_zone.note_on_gain.fixed_velocity = 64;
    const PlaybackProgramSnapshot fixed_program = programFor(&fixed_velocity_zone);
    assert(resolver.resolveNoteOn(fixed_program, 2, 64, 127, 23, &batch) == ZoneResolveStatus::Ok);
    assert(batch.voices[0].velocity == 64);
    assert(sameFilter(batch.voices[0].lowpass, velocity_shifted));

    assert(resolver.resolveNoteOn(fixed_program, 2, 96, 127, 24, &batch) == ZoneResolveStatus::Ok);
    assert(batch.voices[0].velocity == 64);
    assert(!sameFilter(batch.voices[0].lowpass, velocity_shifted));
    assert(batch.voices[0].lowpass.a0_q24 > velocity_shifted.a0_q24);
}

void testInvalidGenericRouteRemainsSoftCompatibilityIssue() {
    NativeResolvedZone native = baseNativeZone();
    ResolvedZoneModulation modulation{};
    modulation.initial_filter_fc_note_on_routes[0] =
        Sf2NoteOnSourceRoute{0x028Au, 500};  // CC10 is live, not a NoteOn source.
    modulation.initial_filter_fc_note_on_route_count = 1;

    PlaybackZone zone{};
    const auto result = Sf2PlaybackMaterializer{}.materialize(native, modulation, &zone);
    assert(result.playablePreview());
    assert(result.hasIssue(PlaybackIssueCustomModulatorsPending));
    assert(zone.note_on_filter.additional_cutoff_route_count == 0);
    assert(zone.valid());
}

void testGenericPreparedRoutesMaterializeWithoutDroppingPanBoundary() {
    NativeResolvedZone native = baseNativeZone();
    ResolvedZoneModulation modulation{};
    assert(addPreparedNoteOnModulator(
        &modulation.prepared_note_on_routes,
        Sf2PreparedNoteOnModulator{0x0002u, 48u, 120, 0u}));
    modulation.supported_prepared_note_on_records = 1;

    PlaybackZone zone{};
    const auto supported = Sf2PlaybackMaterializer{}.materialize(native, modulation, &zone);
    assert(supported.playablePreview());
    assert(!supported.hasIssue(PlaybackIssueCustomModulatorsPending));
    assert(zone.prepared_note_on_modulators.count == 1);
    assert(zone.prepared_note_on_modulators.routes[0].destination_oper == 48u);
    assert(zone.valid());

    ResolvedZoneModulation pan_pending{};
    assert(addPreparedNoteOnModulator(
        &pan_pending.prepared_note_on_routes,
        Sf2PreparedNoteOnModulator{0x0203u, 17u, 250, 0u}));
    const auto pending = Sf2PlaybackMaterializer{}.materialize(native, pan_pending, &zone);
    assert(pending.playablePreview());
    assert(pending.hasIssue(PlaybackIssueCustomModulatorsPending));
    assert(zone.prepared_note_on_modulators.count == 0);
    assert(zone.valid());
}

void testGenericPreparedPitchFilterGainAndFineTuneRunAtNoteOn() {
    PlaybackZone zone{};
    zone.sample_id = 1;
    zone.root_key = 60;
    zone.key_tracking = false;
    zone.root_phase_increment_q32 = kPhaseOneQ32;
    zone.note_on_gain.enabled = true;
    zone.note_on_gain.velocity_to_attenuation_cb = 0;
    zone.mod_envelope.enabled = true;
    zone.mod_envelope.envelope.attack_frames = 1000;
    zone.mod_envelope.envelope.decay_frames = 1000;
    zone.mod_envelope.envelope.release_frames = 1000;
    assert(addPreparedNoteOnModulator(
        &zone.prepared_note_on_modulators,
        Sf2PreparedNoteOnModulator{0x0002u, 7u, 120, 0u}));
    assert(addPreparedNoteOnModulator(
        &zone.prepared_note_on_modulators,
        Sf2PreparedNoteOnModulator{0x0002u, 11u, 300, 0u}));
    assert(addPreparedNoteOnModulator(
        &zone.prepared_note_on_modulators,
        Sf2PreparedNoteOnModulator{0x0002u, 48u, 60, 0u}));
    assert(addPreparedNoteOnModulator(
        &zone.prepared_note_on_modulators,
        Sf2PreparedNoteOnModulator{0x0002u, 52u, 25, 0u}));
    assert(zone.valid());

    const PlaybackProgramSnapshot program = programFor(&zone);
    ZoneResolver resolver;
    ResolvedVoiceBatch batch{};
    assert(resolver.resolveNoteOn(program, 2, 60, 127, 30, &batch) == ZoneResolveStatus::Ok);
    assert(batch.count == 1);
    assert(batch.voices[0].phase_increment_q32 == ZoneResolver::transposeCents(kPhaseOneQ32, 25));
    assert(batch.voices[0].mod_envelope.enabled);
    assert(batch.voices[0].mod_envelope.pitch_depth_cents == 119);
    assert(batch.voices[0].mod_envelope.filter_depth_cents == 298);
    const std::int32_t expected_amplitude = attenuationCbToGainQ15(60);
    const std::int32_t expected_gain = multiplyQ15(
        expected_amplitude,
        sf2PanChannelGainQ15(0, true));
    assert(batch.voices[0].gain_left_q15 == expected_gain);
    assert(batch.voices[0].gain_right_q15 == expected_gain);
}

void testGenericPreparedEnvelopeTimeDestinationsRunAtNoteOn() {
    PlaybackZone zone{};
    zone.sample_id = 1;
    zone.root_key = 60;
    zone.root_phase_increment_q32 = kPhaseOneQ32;
    zone.note_on_gain.enabled = true;
    zone.note_on_gain.velocity_to_attenuation_cb = 0;
    zone.envelope.attack_frames = 1000;
    zone.envelope.decay_frames = 1000;
    zone.envelope.release_frames = 1000;
    zone.mod_envelope.enabled = true;
    zone.mod_envelope.envelope.attack_frames = 1000;
    zone.mod_envelope.envelope.decay_frames = 1000;
    zone.mod_envelope.envelope.release_frames = 1000;
    assert(addPreparedNoteOnModulator(
        &zone.prepared_note_on_modulators,
        Sf2PreparedNoteOnModulator{0x0002u, 28u, 1200, 0u}));
    assert(addPreparedNoteOnModulator(
        &zone.prepared_note_on_modulators,
        Sf2PreparedNoteOnModulator{0x0002u, 34u, 1200, 0u}));
    assert(addPreparedNoteOnModulator(
        &zone.prepared_note_on_modulators,
        Sf2PreparedNoteOnModulator{0x0002u, 36u, 1200, 0u}));
    assert(addPreparedNoteOnModulator(
        &zone.prepared_note_on_modulators,
        Sf2PreparedNoteOnModulator{0x0002u, 38u, 1200, 0u}));
    assert(zone.valid());

    const PlaybackProgramSnapshot program = programFor(&zone);
    ZoneResolver resolver;
    ResolvedVoiceBatch batch{};
    assert(resolver.resolveNoteOn(program, 2, 60, 127, 31, &batch) == ZoneResolveStatus::Ok);
    assert(batch.count == 1);
    assert(batch.voices[0].envelope.attack_frames == 1989);
    assert(batch.voices[0].envelope.decay_frames == 1989);
    assert(batch.voices[0].envelope.release_frames == 1989);
    assert(batch.voices[0].mod_envelope.enabled);
    assert(batch.voices[0].mod_envelope.envelope.decay_frames == 1989);
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
    testGenericNoteOnFilterSourcesMaterializeAndRun();
    testInvalidGenericRouteRemainsSoftCompatibilityIssue();
    testGenericPreparedRoutesMaterializeWithoutDroppingPanBoundary();
    testGenericPreparedPitchFilterGainAndFineTuneRunAtNoteOn();
    testGenericPreparedEnvelopeTimeDestinationsRunAtNoteOn();
    testLegacyPlaybackZoneKeepsPreparedStaticGain();
    std::cout << "SF2 note-on modulation runtime checks passed\n";
    return 0;
}
