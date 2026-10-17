#include "../../firmware/sf2/Sf2ModulationMath.h"
#include "../../firmware/sf2/Sf2PanMath.h"
#include "../../firmware/voice/VoiceRenderer.h"

#include <cstdint>
#include <cstdio>

using gm_synth::sf2::attenuationCbToGainQ15;
using gm_synth::sf2::channelEffectsSendSf2Units;
using gm_synth::sf2::controllerToEffectsSendSf2Units;
using gm_synth::sf2::controllerToPanSf2Units;
using gm_synth::sf2::noteOnGainQ15;
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