#include "../../firmware/gm/PlaybackProgramSnapshot.h"
#include "../../firmware/gm/ZoneResolver.h"
#include "../../firmware/sf2/Sf2PlaybackMaterializer.h"
#include "../../firmware/sf2/Sf2ResonantFilterMath.h"

#include <cassert>
#include <cmath>
#include <complex>
#include <cstdint>
#include <iostream>

namespace {

using gm_synth::gm::MidiChannelState;
using gm_synth::gm::PlaybackProgramSnapshot;
using gm_synth::gm::PlaybackZone;
using gm_synth::gm::ResolvedVoiceBatch;
using gm_synth::gm::ZoneResolveStatus;
using gm_synth::gm::ZoneResolver;
using gm_synth::sf2::NativeResolvedZone;
using gm_synth::sf2::PlaybackIssueFilterPending;
using gm_synth::sf2::ResolvedZoneModulation;
using gm_synth::sf2::Sf2PlaybackMaterializer;
using gm_synth::sf2::prepareResonantLowpassQ24;
using gm_synth::sf2::resonanceDcAttenuationCb;
using gm_synth::voice::BiquadLowpassConfig;
using gm_synth::voice::kPhaseOneQ32;

constexpr double kPi = 3.14159265358979323846;
constexpr double kQ24Scale = static_cast<double>(std::uint32_t{1} << 24u);

NativeResolvedZone baseNativeZone() {
    NativeResolvedZone zone{};
    zone.bank = 0;
    zone.program = 0;
    zone.sample_id = 0;
    zone.key_low = 0;
    zone.key_high = 127;
    zone.velocity_low = 1;
    zone.velocity_high = 127;
    zone.sample_start = 0;
    zone.sample_end = 256;
    zone.loop_start = 0;
    zone.loop_end = 256;
    zone.shdr_start = 0;
    zone.shdr_end = 256;
    zone.shdr_loop_start = 0;
    zone.shdr_loop_end = 256;
    zone.sample_rate = 44100;
    zone.sample_root_key = 60;
    zone.sample_type = 1;
    zone.generators.set(54, 1);
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

double absoluteCentsToHz(std::int32_t cents) {
    return 8.176 * std::pow(2.0, static_cast<double>(cents) / 1200.0);
}

double magnitudeAtHz(const BiquadLowpassConfig& filter, double hz) {
    assert(filter.enabled);
    const double a0 = filter.a0_q24 / kQ24Scale;
    const double a1 = filter.a1_q24 / kQ24Scale;
    const double feedback1 = filter.b1_q24 / kQ24Scale;
    const double feedback2 = filter.b2_q24 / kQ24Scale;
    const double omega = 2.0 * kPi * hz / 44100.0;
    const std::complex<double> z1 = std::exp(std::complex<double>(0.0, -omega));
    const std::complex<double> z2 = z1 * z1;
    const std::complex<double> numerator = a0 + a1 * z1 + a0 * z2;
    const std::complex<double> denominator = 1.0 + feedback1 * z1 + feedback2 * z2;
    return std::abs(numerator / denominator);
}

void testMaterializerQualifiesResonanceAt44100Only() {
    NativeResolvedZone native = baseNativeZone();
    native.generators.set(8, 9000);
    native.generators.set(9, 100);

    PlaybackZone zone{};
    const auto result = Sf2PlaybackMaterializer{}.materialize(
        native,
        ResolvedZoneModulation{},
        &zone);
    assert(result.playablePreview());
    assert(!result.hasIssue(PlaybackIssueFilterPending));
    assert(zone.note_on_filter.resonance_cb == 100);
    assert(zone.gain_left_q15 >= 18400 && zone.gain_left_q15 <= 18450);
    assert(zone.gain_right_q15 == zone.gain_left_q15);

    const auto other_rate = Sf2PlaybackMaterializer{48000}.materialize(
        native,
        ResolvedZoneModulation{},
        &zone);
    assert(other_rate.playablePreview());
    assert(other_rate.hasIssue(PlaybackIssueFilterPending));
}

void testTenDbResonanceHasExpectedPeakAndGainCompensation() {
    BiquadLowpassConfig filter{};
    constexpr std::int32_t kCutoffCents = 6900;
    constexpr std::int32_t kResonanceCb = 100;
    assert(prepareResonantLowpassQ24(
        kCutoffCents,
        kResonanceCb,
        44100,
        &filter));

    // Raw filter Q for 100 cB is 10^(10 dB / 20) ~= 3.162 at Fc.
    const double raw_peak = magnitudeAtHz(filter, absoluteCentsToHz(kCutoffCents));
    assert(raw_peak > 3.05 && raw_peak < 3.28);

    // SF2 applies -Q/2 = -5 dB DC gain compensation. The absolute peak is
    // therefore about +5 dB ~= 1.778 rather than +10 dB.
    assert(resonanceDcAttenuationCb(kResonanceCb) == 50);
    const double compensation = std::pow(10.0, -5.0 / 20.0);
    const double compensated_peak = raw_peak * compensation;
    assert(compensated_peak > 1.70 && compensated_peak < 1.85);
}

PlaybackZone resonantDynamicZone() {
    PlaybackZone zone{};
    zone.sample_id = 0;
    zone.root_key = 60;
    zone.key_tracking = false;
    zone.root_phase_increment_q32 = kPhaseOneQ32;

    zone.note_on_gain.enabled = true;
    zone.note_on_gain.initial_attenuation_cb = 0;
    zone.note_on_gain.velocity_to_attenuation_cb = 0;
    zone.note_on_gain.volume_to_attenuation_cb = 0;
    zone.note_on_gain.pan_amount = 0;
    zone.note_on_gain.expression_to_attenuation_cb = 0;

    zone.note_on_filter.enabled = true;
    zone.note_on_filter.initial_cutoff_cents = 9000;
    zone.note_on_filter.velocity_to_cutoff_cents = 0;
    zone.note_on_filter.resonance_cb = 100;

    zone.mod_lfo.enabled = true;
    zone.mod_lfo.phase_step_q32 = std::uint32_t{1} << 26u;
    zone.mod_lfo.filter_depth_cents = 1200;

    zone.mod_envelope.enabled = true;
    zone.mod_envelope.filter_depth_cents = 600;
    zone.mod_envelope.envelope.sustain_q15 = 32767;
    return zone;
}

void testResolverCombinesResonanceWithDynamicCutoffAndRealtimeGainState() {
    PlaybackZone zone = resonantDynamicZone();
    const PlaybackProgramSnapshot program = programFor(&zone);
    assert(program.valid());

    MidiChannelState state{};
    state.resetAll();
    state.volume = 127;
    state.expression = 127;
    state.pan = 64;

    ZoneResolver resolver;
    ResolvedVoiceBatch batch{};
    assert(resolver.resolveNoteOn(program, state, 2, 60, 100, 1, &batch) ==
           ZoneResolveStatus::Ok);
    assert(batch.count == 1);
    const auto& params = batch.voices[0];

    BiquadLowpassConfig expected_base{};
    assert(prepareResonantLowpassQ24(9000, 100, 44100, &expected_base));
    assert(params.lowpass.a0_q24 == expected_base.a0_q24);
    assert(params.lowpass.a1_q24 == expected_base.a1_q24);
    assert(params.lowpass.b1_q24 == expected_base.b1_q24);
    assert(params.lowpass.b2_q24 == expected_base.b2_q24);

    // Mod-LFO spans -1200..+1200 and Mod-Env adds 0..+600, so the total
    // dynamic offset table must cover -1200..+1800 with the same Q at every point.
    assert(params.dynamic_lowpass_table.enabled);
    assert(params.dynamic_lowpass_table.min_offset_cents == -1200);
    assert(params.dynamic_lowpass_table.max_offset_cents == 1800);
    BiquadLowpassConfig expected_low{};
    BiquadLowpassConfig expected_high{};
    assert(prepareResonantLowpassQ24(7800, 100, 44100, &expected_low));
    assert(prepareResonantLowpassQ24(10800, 100, 44100, &expected_high));
    assert(params.dynamic_lowpass_table.points.front().a0_q24 == expected_low.a0_q24);
    assert(params.dynamic_lowpass_table.points.front().b2_q24 == expected_low.b2_q24);
    assert(params.dynamic_lowpass_table.points.back().a0_q24 == expected_high.a0_q24);
    assert(params.dynamic_lowpass_table.points.back().b2_q24 == expected_high.b2_q24);

    // 100 cB resonance contributes 50 cB to the effective attenuation node.
    // It is stored in realtime_control so later CC7/CC11 refreshes preserve it.
    assert(params.realtime_control.initial_attenuation_cb == 50);
    assert(params.gain_left_q15 >= 18400 && params.gain_left_q15 <= 18450);
    assert(params.gain_right_q15 == params.gain_left_q15);
}

void testFull96DbRangeRemainsRepresentable() {
    BiquadLowpassConfig low_corner{};
    BiquadLowpassConfig high_corner{};
    assert(prepareResonantLowpassQ24(1500, 960, 44100, &low_corner));
    assert(prepareResonantLowpassQ24(13500, 960, 44100, &high_corner));
    assert(low_corner.valid() && high_corner.valid());
    assert(low_corner.a0_q24 > 0);
    assert(high_corner.a0_q24 > low_corner.a0_q24);
    assert(resonanceDcAttenuationCb(960) == 480);
}

}  // namespace

int main() {
    testMaterializerQualifiesResonanceAt44100Only();
    testTenDbResonanceHasExpectedPeakAndGainCompensation();
    testResolverCombinesResonanceWithDynamicCutoffAndRealtimeGainState();
    testFull96DbRangeRemainsRepresentable();
    std::cout << "SF2 filter resonance runtime checks passed\n";
    return 0;
}
