#include "../../firmware/sf2/Sf2ModulationMath.h"
#include "../../firmware/voice/VoiceRenderer.h"

#include <cstdint>
#include <cstdio>

using gm_synth::sf2::attenuationCbToGainQ15;
using gm_synth::sf2::channelEffectsSendSf2Units;
using gm_synth::sf2::controllerToEffectsSendSf2Units;
using gm_synth::sf2::noteOnGainQ15;
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

void test_centibel_gain_is_fixed_point_and_clamped() {
    CHECK(attenuationCbToGainQ15(-200) == kQ15Unity);
    CHECK(attenuationCbToGainQ15(0) == kQ15Unity);
    CHECK(attenuationCbToGainQ15(200) == 3277);   // -20 dB => 0.1 amplitude
    CHECK(attenuationCbToGainQ15(400) == 328);    // -40 dB => 0.01 amplitude
    CHECK(attenuationCbToGainQ15(1440) == 0);
    CHECK(attenuationCbToGainQ15(5000) == 0);
}

void test_note_on_gain_sums_before_clamp_and_then_pans() {
    std::int32_t left = 0;
    std::int32_t right = 0;

    // Negative preset attenuation may cancel part of velocity attenuation;
    // clamping it before modulation would incorrectly lose this behavior.
    noteOnGainQ15(-100, 960, 63, 0, &left, &right);
    CHECK(left == attenuationCbToGainQ15(22));
    CHECK(right == left);

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
    test_centibel_gain_is_fixed_point_and_clamped();
    test_note_on_gain_sums_before_clamp_and_then_pans();
    test_effects_send_controller_is_positive_unipolar_and_clamped();

    if (failures != 0) {
        std::fprintf(stderr, "%d SF2 modulation-math checks failed\n", failures);
        return 1;
    }
    std::printf("SF2 modulation-math host checks passed\n");
    return 0;
}
