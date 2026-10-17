#include "../../firmware/sf2/Sf2ModulationMath.h"
#include "../../firmware/sf2/Sf2PanMath.h"
#include "../../firmware/voice/VoiceRenderer.h"

#include <cstdint>
#include <cstdio>

using gm_synth::sf2::Sf2SourceCurve;
using gm_synth::sf2::Sf2SourceDescriptor;
using gm_synth::sf2::attenuationCbToGainQ15;
using gm_synth::sf2::channelEffectsSendSf2Units;
using gm_synth::sf2::controllerToEffectsSendSf2Units;
using gm_synth::sf2::controllerToPanSf2Units;
using gm_synth::sf2::decodeSf2SourceOper;
using gm_synth::sf2::isSupportedSf2NoteOnSourceOper;
using gm_synth::sf2::noteOnGainQ15;
using gm_synth::sf2::noteOnSourceToAmount;
using gm_synth::sf2::positiveLinearToAmount;
using gm_synth::sf2::sf2PanChannelGainQ15;
using gm_synth::sf2::velocityToAttenuationCb;
using gm_synth::voice::kQ15Unity;

namespace {

int failures = 0;

#define CHECK(expr)                                                                    \
    do {                                                                               \
        if (!(expr)) {                                                                 \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #expr);     \
            ++failures;                                                                \
        }                                                                              \
    } while (0)

std::int32_t multiplyQ15(std::int32_t a, std::int32_t b) {
    return static_cast<std::int32_t>(
        (static_cast<std::int64_t>(a) * b + (std::int64_t{1} << 14)) >> 15);
}

void test_default_velocity_curve_matches_sf2_reference_points() {
    CHECK(velocityToAttenuationCb(127, 960) == 0);
    CHECK(velocityToAttenuationCb(111, 960) == 23);
    CHECK(velocityToAttenuationCb(95, 960) == 50);
    CHECK(velocityToAttenuationCb(63, 960) == 122);
    CHECK(velocityToAttenuationCb(15, 960) == 371);
    CHECK(velocityToAttenuationCb(0, 960) == 960);

    // A zero-amount instrument override cancels the mandatory default route.
    CHECK(velocityToAttenuationCb(32, 0) == 0);
    // Preset-additive modulation can make the effective route signed.
    CHECK(velocityToAttenuationCb(0, -120) == -120);
}

void test_positive_linear_controller_uses_128_step_domain() {
    CHECK(positiveLinearToAmount(0, 50) == 0);
    CHECK(positiveLinearToAmount(64, 50) == 25);
    CHECK(positiveLinearToAmount(127, 50) == 50);
    CHECK(positiveLinearToAmount(64, -50) == -25);
    CHECK(positiveLinearToAmount(127, 200) == 198);
}

void test_generic_source_decoder_matches_sf2_bit_layout() {
    Sf2SourceDescriptor source{};

    // NoteOn key number: positive bipolar linear.
    CHECK(decodeSf2SourceOper(0x0203u, &source));
    CHECK(source.index == 3u);
    CHECK(!source.midi_cc);
    CHECK(!source.negative);
    CHECK(source.bipolar);
    CHECK(source.curve == Sf2SourceCurve::Linear);
    CHECK(isSupportedSf2NoteOnSourceOper(0x0203u));

    // NoteOn velocity: negative unipolar convex.
    CHECK(decodeSf2SourceOper(0x0902u, &source));
    CHECK(source.index == 2u);
    CHECK(!source.midi_cc);
    CHECK(source.negative);
    CHECK(!source.bipolar);
    CHECK(source.curve == Sf2SourceCurve::Convex);
    CHECK(isSupportedSf2NoteOnSourceOper(0x0902u));

    // CC10 is structurally valid but is a live controller, not an immutable
    // NoteOn source for this evaluator.
    CHECK(decodeSf2SourceOper(0x028Au, &source));
    CHECK(source.midi_cc);
    CHECK(!isSupportedSf2NoteOnSourceOper(0x028Au));

    // Type values above switch are reserved/unsupported.
    CHECK(!decodeSf2SourceOper(0x1002u, &source));
}

void test_generic_linear_note_on_sources_keep_128_step_semantics() {
    // GeneralUser's key-number route: positive bipolar linear, amount 1200.
    CHECK(noteOnSourceToAmount(0x0203u, 0, 100, 1200) == -1200);
    CHECK(noteOnSourceToAmount(0x0203u, 32, 100, 1200) == -600);
    CHECK(noteOnSourceToAmount(0x0203u, 64, 100, 1200) == 0);
    CHECK(noteOnSourceToAmount(0x0203u, 96, 100, 1200) == 600);
    CHECK(noteOnSourceToAmount(0x0203u, 127, 100, 1200) == 1181);
    CHECK(noteOnSourceToAmount(0x0203u, 96, 100, -1200) == -600);

    // Negative-unipolar linear retains the already-qualified velocity-filter
    // endpoint rule used elsewhere in the engine.
    CHECK(noteOnSourceToAmount(0x0102u, 60, 0, -2400) == -2400);
    CHECK(noteOnSourceToAmount(0x0102u, 60, 64, -2400) == -1200);
    CHECK(noteOnSourceToAmount(0x0102u, 60, 127, -2400) == 0);
}

void test_generic_nonlinear_note_on_sources_follow_direction_and_curve() {
    // GeneralUser's negative-unipolar convex velocity route must fall from a
    // full low-velocity contribution to zero at velocity 127.
    const std::int32_t convex_low = noteOnSourceToAmount(0x0902u, 60, 0, 1200);
    const std::int32_t convex_mid = noteOnSourceToAmount(0x0902u, 60, 64, 1200);
    const std::int32_t convex_high = noteOnSourceToAmount(0x0902u, 60, 127, 1200);
    CHECK(convex_low == 1200);
    CHECK(convex_mid > 0 && convex_mid < convex_low);
    CHECK(convex_high == 0);

    // Positive concave and positive convex have opposite curvature while both
    // remain monotonic and bounded in the SF2 7-bit domain.
    const std::int32_t concave_mid = noteOnSourceToAmount(0x0402u, 60, 64, 1000);
    const std::int32_t convex_positive_mid = noteOnSourceToAmount(0x0802u, 60, 64, 1000);
    CHECK(concave_mid > convex_positive_mid);
    CHECK(concave_mid > 0 && concave_mid < 1000);
    CHECK(convex_positive_mid > 0 && convex_positive_mid < 1000);

    // Positive-unipolar switch toggles at the half-domain boundary.
    CHECK(noteOnSourceToAmount(0x0C02u, 60, 63, 400) == 0);
    CHECK(noteOnSourceToAmount(0x0C02u, 60, 64, 400) > 390);

    // Unsupported live-controller identities contribute nothing through the
    // immutable NoteOn evaluator.
    CHECK(noteOnSourceToAmount(0x028Au, 60, 64, 500) == 0);
}

void test_default_cc10_uses_full_pan_range_without_early_clamp() {
    CHECK(controllerToPanSf2Units(0) == -500);
    CHECK(controllerToPanSf2Units(32) == -250);
    CHECK(controllerToPanSf2Units(64) == 0);
    CHECK(controllerToPanSf2Units(96) == 250);
    CHECK(controllerToPanSf2Units(127) == 492);
}

void test_centibel_gain_is_fixed_point_and_clamped() {
    CHECK(attenuationCbToGainQ15(-200) == kQ15Unity);
    CHECK(attenuationCbToGainQ15(0) == kQ15Unity);
    CHECK(attenuationCbToGainQ15(200) == 3277);   // -20 dB => 0.1 amplitude
    CHECK(attenuationCbToGainQ15(400) == 328);    // -40 dB => 0.01 amplitude
    CHECK(attenuationCbToGainQ15(1440) == 0);
    CHECK(attenuationCbToGainQ15(5000) == 0);
}

void test_equal_power_pan_has_expected_endpoints_and_center() {
    CHECK(sf2PanChannelGainQ15(-500, true) == kQ15Unity);
    CHECK(sf2PanChannelGainQ15(-500, false) == 0);
    CHECK(sf2PanChannelGainQ15(500, true) == 0);
    CHECK(sf2PanChannelGainQ15(500, false) == kQ15Unity);
    CHECK(sf2PanChannelGainQ15(0, true) == 23170);
    CHECK(sf2PanChannelGainQ15(0, false) == 23170);
    CHECK(sf2PanChannelGainQ15(-250, true) > sf2PanChannelGainQ15(-250, false));
    CHECK(sf2PanChannelGainQ15(250, true) < sf2PanChannelGainQ15(250, false));
}

void test_note_on_gain_sums_before_clamp_and_then_pans() {
    std::int32_t left = 0;
    std::int32_t right = 0;

    // Negative preset attenuation may cancel part of velocity attenuation;
    // clamping it before modulation would incorrectly lose this behavior.
    noteOnGainQ15(-100, 960, 63, 0, &left, &right);
    const std::int32_t amplitude = attenuationCbToGainQ15(22);
    const std::int32_t centered = multiplyQ15(amplitude, 23170);
    CHECK(left == centered);
    CHECK(right == centered);

    noteOnGainQ15(0, 0, 127, -500, &left, &right);
    CHECK(left == kQ15Unity);
    CHECK(right == 0);

    noteOnGainQ15(0, 0, 127, 500, &left, &right);
    CHECK(left == 0);
    CHECK(right == kQ15Unity);
}

void test_effects_send_controller_is_positive_unipolar_and_clamped() {
    CHECK(controllerToEffectsSendSf2Units(0, 200) == 0);
    CHECK(controllerToEffectsSendSf2Units(64, 200) == 100);
    CHECK(controllerToEffectsSendSf2Units(127, 200) == 198);
    CHECK(controllerToEffectsSendSf2Units(127, 300) == 298);
    CHECK(controllerToEffectsSendSf2Units(64, -200) == -100);

    CHECK(channelEffectsSendSf2Units(100, 64, 200) == 200);
    CHECK(channelEffectsSendSf2Units(50, 127, 300) == 348);
    CHECK(channelEffectsSendSf2Units(950, 127, 200) == 1000);
    CHECK(channelEffectsSendSf2Units(20, 127, -200) == 0);
}

}  // namespace

int main() {
    test_default_velocity_curve_matches_sf2_reference_points();
    test_positive_linear_controller_uses_128_step_domain();
    test_generic_source_decoder_matches_sf2_bit_layout();
    test_generic_linear_note_on_sources_keep_128_step_semantics();
    test_generic_nonlinear_note_on_sources_follow_direction_and_curve();
    test_default_cc10_uses_full_pan_range_without_early_clamp();
    test_centibel_gain_is_fixed_point_and_clamped();
    test_equal_power_pan_has_expected_endpoints_and_center();
    test_note_on_gain_sums_before_clamp_and_then_pans();
    test_effects_send_controller_is_positive_unipolar_and_clamped();

    if (failures != 0) {
        std::fprintf(stderr, "%d SF2 modulation-math checks failed\n", failures);
        return 1;
    }
    std::printf("SF2 modulation-math host checks passed\n");
    return 0;
}
