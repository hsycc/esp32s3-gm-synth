#include "../../firmware/sf2/Sf2PlaybackMaterializer.h"
#include "../../firmware/voice/PitchMath.h"

#include <cassert>
#include <cmath>
#include <cstdint>
#include <iostream>

namespace {

using gm_synth::gm::PlaybackZone;
using gm_synth::sf2::NativeResolvedZone;
using gm_synth::sf2::NativeZoneFlagAddressOutsideSampleHeader;
using gm_synth::sf2::NativeZoneFlagIgnoredInvalidPresetGenerator;
using gm_synth::sf2::NativeZoneFlagInvalidLoopRange;
using gm_synth::sf2::NativeZoneFlagRomSample;
using gm_synth::sf2::NativeZoneFlagStereoOrLinkedSample;
using gm_synth::sf2::PlaybackIssueAddressOutsideResidentSample;
using gm_synth::sf2::PlaybackIssueCustomModulatorsPending;
using gm_synth::sf2::PlaybackIssueDefaultModulatorsPending;
using gm_synth::sf2::PlaybackIssueEffectsSendPending;
using gm_synth::sf2::PlaybackIssueFilterPending;
using gm_synth::sf2::PlaybackIssueFixedVelocityPending;
using gm_synth::sf2::PlaybackIssueGainAboveUnityClamped;
using gm_synth::sf2::PlaybackIssueIgnoredInvalidPresetGenerator;
using gm_synth::sf2::PlaybackIssueInvalidLoop;
using gm_synth::sf2::PlaybackIssueLfoPending;
using gm_synth::sf2::PlaybackIssueModEnvelopePending;
using gm_synth::sf2::PlaybackIssuePanLawApproximation;
using gm_synth::sf2::PlaybackIssueRomSample;
using gm_synth::sf2::PlaybackIssueScaleTuningPending;
using gm_synth::sf2::PlaybackIssueStereoOrLinkedSample;
using gm_synth::sf2::PlaybackIssueUnsupportedSampleMode;
using gm_synth::sf2::PlaybackMaterializationStatus;
using gm_synth::sf2::ResolvedZoneModulation;
using gm_synth::sf2::Sf2PlaybackMaterializer;
using gm_synth::voice::LoopMode;
using gm_synth::voice::kPhaseOneQ32;
using gm_synth::voice::kQ15Unity;

NativeResolvedZone baseZone() {
    NativeResolvedZone zone{};
    zone.bank = 0;
    zone.program = 0;
    zone.sample_id = 1;
    zone.key_low = 12;
    zone.key_high = 100;
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
    zone.sample_pitch_correction_cents = 0;
    zone.sample_type = 1;
    return zone;
}

void testSimpleZoneBecomesExplicitPreview() {
    Sf2PlaybackMaterializer materializer;
    PlaybackZone output{};
    const auto result = materializer.materialize(baseZone(), &output);

    assert(result.status == PlaybackMaterializationStatus::Preview);
    assert(result.playablePreview());
    assert(!result.hasIssue(PlaybackIssueDefaultModulatorsPending));
    assert(result.hasIssue(PlaybackIssueEffectsSendPending));
    assert(output.sample_id == 1);
    assert(output.key_low == 12);
    assert(output.key_high == 100);
    assert(output.velocity_low == 1);
    assert(output.velocity_high == 127);
    assert(output.root_key == 60);
    assert(output.key_tracking);
    assert(output.root_phase_increment_q32 == kPhaseOneQ32);
    assert(output.loop_mode == LoopMode::None);
    assert(!output.sample_region.enabled);
    assert(output.gain_left_q15 == kQ15Unity);
    assert(output.gain_right_q15 == kQ15Unity);
    assert(output.envelope.delay_frames == 43);
    assert(output.envelope.attack_frames == 43);
    assert(output.envelope.hold_frames == 43);
    assert(output.envelope.decay_frames == 43);
    assert(output.envelope.sustain_q15 == kQ15Unity);
    assert(output.envelope.release_frames == 43);
    assert(output.note_on_filter.enabled);
    assert(output.note_on_filter.initial_cutoff_cents == 13500);
    assert(output.note_on_filter.velocity_to_cutoff_cents == -2400);
    assert(output.note_on_filter.resonance_cb == 0);
    assert(!result.hasIssue(PlaybackIssueFilterPending));
    assert(output.vibrato_lfo.enabled);
    assert(output.vibrato_lfo.delay_frames == 43);
    assert(output.vibrato_lfo.phase_step_q32 != 0);
    assert(output.vibrato_lfo.static_pitch_depth_cents == 0);
    assert(output.vibrato_lfo.channel_pressure_to_pitch_cents == 50);
    assert(output.vibrato_lfo.modulation_wheel_to_pitch_cents == 50);
    assert(!output.mod_envelope.enabled);
    assert(output.valid());
}

void testResolvedAddressWindowAndLoopAreRelativeToResidentSample() {
    NativeResolvedZone native = baseZone();
    native.sample_start = 110;
    native.sample_end = 190;
    native.loop_start = 130;
    native.loop_end = 170;
    native.generators.set(54, 1);

    PlaybackZone output{};
    const auto result = Sf2PlaybackMaterializer{}.materialize(native, &output);
    assert(result.status == PlaybackMaterializationStatus::Preview);
    assert(output.loop_mode == LoopMode::Continuous);
    assert(output.sample_region.enabled);
    assert(output.sample_region.start_frame == 10);
    assert(output.sample_region.end_frame == 90);
    assert(output.sample_region.loop_start_frame == 30);
    assert(output.sample_region.loop_end_frame == 70);
    assert(output.valid());

    native.generators.set(54, 3);
    const auto release_loop = Sf2PlaybackMaterializer{}.materialize(native, &output);
    assert(release_loop.playablePreview());
    assert(output.loop_mode == LoopMode::UntilRelease);
}

void testPitchMaterializationIncludesRateTuneAndCorrection() {
    NativeResolvedZone native = baseZone();
    native.sample_rate = 22050;
    native.sample_pitch_correction_cents = 5;
    native.generators.set(51, 1);
    native.generators.set(52, -5);

    PlaybackZone output{};
    const auto result = Sf2PlaybackMaterializer{}.materialize(native, &output);
    assert(result.playablePreview());
    const std::uint64_t expected = gm_synth::voice::phaseIncrementQ32(22050, 44100, 100.0);
    assert(output.root_phase_increment_q32 == expected);
    assert(output.key_tracking);
}

void testFixedKeyAndScaleTuningQualification() {
    NativeResolvedZone native = baseZone();
    native.generators.set(46, 64);
    native.generators.set(56, 100);

    PlaybackZone output{};
    auto result = Sf2PlaybackMaterializer{}.materialize(native, &output);
    assert(result.playablePreview());
    assert(!output.key_tracking);
    assert(output.root_phase_increment_q32 ==
           gm_synth::voice::phaseIncrementQ32(44100, 44100, 400.0));

    native = baseZone();
    native.generators.set(56, 0);
    result = Sf2PlaybackMaterializer{}.materialize(native, &output);
    assert(result.playablePreview());
    assert(!output.key_tracking);
    assert(!result.hasIssue(PlaybackIssueScaleTuningPending));

    native.generators.set(56, 50);
    result = Sf2PlaybackMaterializer{}.materialize(native, &output);
    assert(result.playablePreview());
    assert(output.key_tracking);  // preview uses current 100-cent runtime tracking
    assert(result.hasIssue(PlaybackIssueScaleTuningPending));
}

void testEnvelopeFieldsAreConvertedFromNativeSf2Units() {
    NativeResolvedZone native = baseZone();
    native.generators.set(33, -1200);  // 0.5 second delay
    native.generators.set(34, 0);      // 1 second attack
    native.generators.set(35, 1200);   // 2 second hold
    native.generators.set(36, -1200);  // 0.5 second decay
    native.generators.set(37, 600);    // 60 dB sustain attenuation
    native.generators.set(38, 1200);   // 2 seconds release
    native.generators.set(39, 100);
    native.generators.set(40, -50);

    PlaybackZone output{};
    const auto result = Sf2PlaybackMaterializer{}.materialize(native, &output);
    assert(result.playablePreview());
    assert(output.envelope.delay_frames == 22050);
    assert(output.envelope.attack_frames == 44100);
    assert(output.envelope.hold_frames == 88200);
    assert(output.envelope.decay_frames == 22050);
    assert(output.envelope.release_frames == 88200);
    assert(output.envelope.sustain_q15 >= 32 && output.envelope.sustain_q15 <= 33);
    assert(output.envelope_key_scaling.hold_timecents_per_key == 100);
    assert(output.envelope_key_scaling.decay_timecents_per_key == -50);
}

void testStaticFilterVelocityAndResonanceAreMaterialized() {
    NativeResolvedZone native = baseZone();
    native.generators.set(8, 9600);
    ResolvedZoneModulation modulation{};
    modulation.velocity_to_filter_fc_cents = -1200;

    PlaybackZone output{};
    const auto result = Sf2PlaybackMaterializer{}.materialize(native, modulation, &output);
    assert(result.playablePreview());
    assert(!result.hasIssue(PlaybackIssueFilterPending));
    assert(output.note_on_filter.enabled);
    assert(output.note_on_filter.initial_cutoff_cents == 9600);
    assert(output.note_on_filter.velocity_to_cutoff_cents == -1200);
    assert(output.note_on_filter.resonance_cb == 0);

    // 100 cB = 10 dB resonance. SF2 requires half of that amount as DC
    // attenuation, so the legacy static gain is approximately -5 dB.
    native.generators.set(9, 100);
    const auto resonant = Sf2PlaybackMaterializer{}.materialize(native, modulation, &output);
    assert(resonant.playablePreview());
    assert(!resonant.hasIssue(PlaybackIssueFilterPending));
    assert(output.note_on_filter.resonance_cb == 100);
    assert(output.gain_left_q15 >= 18400 && output.gain_left_q15 <= 18450);
    assert(output.gain_right_q15 == output.gain_left_q15);

    const auto other_rate = Sf2PlaybackMaterializer{48000}.materialize(native, modulation, &output);
    assert(other_rate.playablePreview());
    assert(other_rate.hasIssue(PlaybackIssueFilterPending));
}

void testVibratoGeneratorsAndControllerRoutesAreMaterialized() {
    NativeResolvedZone native = baseZone();
    native.generators.set(6, 25);
    native.generators.set(23, 0);    // 1 second delay
    native.generators.set(24, 1200); // one octave above 8.176 Hz
    ResolvedZoneModulation modulation{};
    modulation.channel_pressure_to_vib_lfo_pitch_cents = 30;
    modulation.modulation_wheel_to_vib_lfo_pitch_cents = 70;

    PlaybackZone output{};
    const auto result = Sf2PlaybackMaterializer{}.materialize(native, modulation, &output);
    assert(result.playablePreview());
    assert(!result.hasIssue(PlaybackIssueDefaultModulatorsPending));
    assert(!result.hasIssue(PlaybackIssueLfoPending));
    assert(output.vibrato_lfo.enabled);
    assert(output.vibrato_lfo.delay_frames == 44100);
    assert(output.vibrato_lfo.phase_step_q32 != 0);
    assert(output.vibrato_lfo.static_pitch_depth_cents == 25);
    assert(output.vibrato_lfo.channel_pressure_to_pitch_cents == 30);
    assert(output.vibrato_lfo.modulation_wheel_to_pitch_cents == 70);
}

void testDynamicEnvelopeLfoAndEffectsQualification() {
    NativeResolvedZone native = baseZone();
    native.generators.set(33, 0);
    native.generators.set(35, 0);
    native.generators.set(39, 100);
    native.generators.set(8, 12000);
    native.generators.set(7, -200);
    native.generators.set(11, 1200);
    native.generators.set(25, 0);
    native.generators.set(26, -1200);
    native.generators.set(27, 1200);
    native.generators.set(28, -1200);
    native.generators.set(29, 250);
    native.generators.set(30, 0);
    native.generators.set(31, 50);
    native.generators.set(32, -50);
    native.generators.set(5, 25);
    native.generators.set(10, 600);
    native.generators.set(13, 100);
    native.generators.set(21, 0);
    native.generators.set(15, 300);
    native.generators.set(16, 400);
    native.generators.set(47, 100);
    native.preset_modulator_records = 1;
    native.instrument_modulator_records = 2;

    PlaybackZone output{};
    const auto result = Sf2PlaybackMaterializer{}.materialize(native, &output);
    assert(result.playablePreview());
    assert(output.envelope.delay_frames == 44100);
    assert(output.envelope.hold_frames == 44100);
    assert(output.envelope_key_scaling.hold_timecents_per_key == 100);
    assert(!result.hasIssue(PlaybackIssueFilterPending));
    assert(!result.hasIssue(PlaybackIssueModEnvelopePending));
    assert(!result.hasIssue(PlaybackIssueLfoPending));
    assert(output.mod_envelope.enabled);
    assert(output.mod_envelope.pitch_depth_cents == -200);
    assert(output.mod_envelope.filter_depth_cents == 1200);
    assert(output.mod_envelope.envelope.delay_frames == 44100);
    assert(output.mod_envelope.envelope.attack_frames == 22050);
    assert(output.mod_envelope.envelope.hold_frames == 88200);
    assert(output.mod_envelope.envelope.decay_frames == 22050);
    assert(output.mod_envelope.envelope.sustain_q15 == 24575);
    assert(output.mod_envelope.envelope.release_frames == 44100);
    assert(output.mod_envelope.key_scaling.hold_timecents_per_key == 50);
    assert(output.mod_envelope.key_scaling.decay_timecents_per_key == -50);
    assert(output.mod_lfo.enabled);
    assert(output.mod_lfo.pitch_depth_cents == 25);
    assert(output.mod_lfo.filter_depth_cents == 600);
    assert(output.mod_lfo.volume_depth_cb == 100);
    assert(result.hasIssue(PlaybackIssueEffectsSendPending));
    assert(result.hasIssue(PlaybackIssueFixedVelocityPending));
    assert(result.hasIssue(PlaybackIssueCustomModulatorsPending));
}

void testResolvedControllerAmountsAreCopiedIntoPlaybackZone() {
    NativeResolvedZone native = baseZone();
    ResolvedZoneModulation modulation{};
    modulation.volume_to_attenuation_cb = 700;
    modulation.pan_amount = 800;
    modulation.expression_to_attenuation_cb = 600;

    PlaybackZone output{};
    const auto result = Sf2PlaybackMaterializer{}.materialize(native, modulation, &output);
    assert(result.playablePreview());
    assert(output.note_on_gain.volume_to_attenuation_cb == 700);
    assert(output.note_on_gain.pan_amount == 800);
    assert(output.note_on_gain.expression_to_attenuation_cb == 600);
}

void testStaticPanAndGainRemainExplicitApproximations() {
    NativeResolvedZone native = baseZone();
    native.generators.set(17, 500);
    native.generators.set(48, 60);

    PlaybackZone output{};
    auto result = Sf2PlaybackMaterializer{}.materialize(native, &output);
    assert(result.playablePreview());
    assert(result.hasIssue(PlaybackIssuePanLawApproximation));
    assert(output.gain_left_q15 == 0);
    assert(output.gain_right_q15 > 16000 && output.gain_right_q15 < 17000);

    native = baseZone();
    native.generators.set(48, -100);
    result = Sf2PlaybackMaterializer{}.materialize(native, &output);
    assert(result.playablePreview());
    assert(result.hasIssue(PlaybackIssueGainAboveUnityClamped));
    assert(output.gain_left_q15 == kQ15Unity);
    assert(output.gain_right_q15 == kQ15Unity);
}

void testHardAddressFailureDoesNotMutateOutput() {
    NativeResolvedZone native = baseZone();
    native.sample_start = 90;
    native.flags |= NativeZoneFlagAddressOutsideSampleHeader;

    PlaybackZone output{};
    output.sample_id = 777;
    output.key_low = 17;
    const auto result = Sf2PlaybackMaterializer{}.materialize(native, &output);
    assert(result.status == PlaybackMaterializationStatus::Unrepresentable);
    assert(!result.playablePreview());
    assert(result.hasIssue(PlaybackIssueAddressOutsideResidentSample));
    assert(output.sample_id == 777);
    assert(output.key_low == 17);
}

void testHardLoopSampleModeRomAndStereoFailuresAreExplicit() {
    PlaybackZone output{};

    NativeResolvedZone native = baseZone();
    native.generators.set(54, 1);
    native.flags |= NativeZoneFlagInvalidLoopRange;
    auto result = Sf2PlaybackMaterializer{}.materialize(native, &output);
    assert(result.status == PlaybackMaterializationStatus::Unrepresentable);
    assert(result.hasIssue(PlaybackIssueInvalidLoop));

    native = baseZone();
    native.generators.set(54, 2);
    result = Sf2PlaybackMaterializer{}.materialize(native, &output);
    assert(result.status == PlaybackMaterializationStatus::Unrepresentable);
    assert(result.hasIssue(PlaybackIssueUnsupportedSampleMode));

    native = baseZone();
    native.flags |= NativeZoneFlagRomSample;
    result = Sf2PlaybackMaterializer{}.materialize(native, &output);
    assert(result.status == PlaybackMaterializationStatus::Unrepresentable);
    assert(result.hasIssue(PlaybackIssueRomSample));

    native = baseZone();
    native.flags |= NativeZoneFlagStereoOrLinkedSample;
    result = Sf2PlaybackMaterializer{}.materialize(native, &output);
    assert(result.status == PlaybackMaterializationStatus::Unrepresentable);
    assert(result.hasIssue(PlaybackIssueStereoOrLinkedSample));
}

void testMalformedPresetGeneratorWarningSurvivesMaterialization() {
    NativeResolvedZone native = baseZone();
    native.flags |= NativeZoneFlagIgnoredInvalidPresetGenerator;

    PlaybackZone output{};
    const auto result = Sf2PlaybackMaterializer{}.materialize(native, &output);
    assert(result.playablePreview());
    assert(result.hasIssue(PlaybackIssueIgnoredInvalidPresetGenerator));
}

void testInvalidArgumentsAreTransactional() {
    NativeResolvedZone native = baseZone();
    const auto null_output = Sf2PlaybackMaterializer{}.materialize(native, nullptr);
    assert(null_output.status == PlaybackMaterializationStatus::InvalidArgument);

    PlaybackZone output{};
    output.sample_id = 42;
    const auto zero_rate = Sf2PlaybackMaterializer{0}.materialize(native, &output);
    assert(zero_rate.status == PlaybackMaterializationStatus::InvalidArgument);
    assert(output.sample_id == 42);
}

}  // namespace

int main() {
    testSimpleZoneBecomesExplicitPreview();
    testResolvedAddressWindowAndLoopAreRelativeToResidentSample();
    testPitchMaterializationIncludesRateTuneAndCorrection();
    testFixedKeyAndScaleTuningQualification();
    testEnvelopeFieldsAreConvertedFromNativeSf2Units();
    testStaticFilterVelocityAndResonanceAreMaterialized();
    testVibratoGeneratorsAndControllerRoutesAreMaterialized();
    testDynamicEnvelopeLfoAndEffectsQualification();
    testResolvedControllerAmountsAreCopiedIntoPlaybackZone();
    testStaticPanAndGainRemainExplicitApproximations();
    testHardAddressFailureDoesNotMutateOutput();
    testHardLoopSampleModeRomAndStereoFailuresAreExplicit();
    testMalformedPresetGeneratorWarningSurvivesMaterialization();
    testInvalidArgumentsAreTransactional();
    std::cout << "SF2 playback materializer host checks passed\n";
    return 0;
}
