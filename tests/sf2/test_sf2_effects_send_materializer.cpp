#include "sf2/Sf2PlaybackMaterializer.h"

#include <cassert>
#include <cstdint>
#include <iostream>

namespace {

using gm_synth::gm::PlaybackZone;
using gm_synth::sf2::NativeResolvedZone;
using gm_synth::sf2::PlaybackIssueEffectsSendPending;
using gm_synth::sf2::ResolvedZoneModulation;
using gm_synth::sf2::Sf2PlaybackMaterializer;

NativeResolvedZone baseZone() {
    NativeResolvedZone zone{};
    zone.bank = 0;
    zone.program = 0;
    zone.sample_id = 1;
    zone.key_low = 0;
    zone.key_high = 127;
    zone.velocity_low = 1;
    zone.velocity_high = 127;
    zone.sample_start = 0;
    zone.sample_end = 100;
    zone.loop_start = 10;
    zone.loop_end = 90;
    zone.shdr_start = 0;
    zone.shdr_end = 100;
    zone.shdr_loop_start = 10;
    zone.shdr_loop_end = 90;
    zone.sample_rate = 44100;
    zone.sample_root_key = 60;
    zone.sample_type = 1;
    return zone;
}

void testStaticGeneratorsAndResolvedControllerAmountsArePreserved() {
    NativeResolvedZone native = baseZone();
    native.generators.set(16, 400);  // reverbEffectsSend
    native.generators.set(15, 300);  // chorusEffectsSend

    ResolvedZoneModulation modulation{};
    modulation.reverb_send_amount = 350;
    modulation.chorus_send_amount = 275;

    PlaybackZone output{};
    const auto result = Sf2PlaybackMaterializer{}.materialize(native, modulation, &output);
    assert(result.playablePreview());
    assert(result.hasIssue(PlaybackIssueEffectsSendPending));
    assert(output.effects_send.enabled);
    assert(output.effects_send.reverb_sf2_units == 400);
    assert(output.effects_send.chorus_sf2_units == 300);
    assert(output.effects_send.reverb_cc_amount == 350);
    assert(output.effects_send.chorus_cc_amount == 275);
    assert(output.valid());
}

void testStaticEffectsSendGeneratorsClampToSf2Range() {
    NativeResolvedZone native = baseZone();
    native.generators.set(16, 1400);
    native.generators.set(15, -200);

    ResolvedZoneModulation modulation{};
    PlaybackZone output{};
    const auto result = Sf2PlaybackMaterializer{}.materialize(native, modulation, &output);
    assert(result.playablePreview());
    assert(output.effects_send.reverb_sf2_units == 1000);
    assert(output.effects_send.chorus_sf2_units == 0);
}

}  // namespace

int main() {
    testStaticGeneratorsAndResolvedControllerAmountsArePreserved();
    testStaticEffectsSendGeneratorsClampToSf2Range();
    std::cout << "SF2 effects-send materializer checks passed\n";
    return 0;
}
