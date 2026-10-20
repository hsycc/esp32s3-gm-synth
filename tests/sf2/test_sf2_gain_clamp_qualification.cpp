#include "sf2/Sf2PlaybackMaterializer.h"
#include "sf2/Sf2PanMath.h"
#include "voice/VoiceRenderer.h"

#include <cassert>
#include <cstdint>
#include <iostream>

namespace {

using gm_synth::gm::PlaybackZone;
using gm_synth::sf2::NativeResolvedZone;
using gm_synth::sf2::PlaybackIssueGainAboveUnityClamped;
using gm_synth::sf2::PlaybackMaterializationStatus;
using gm_synth::sf2::Sf2PlaybackMaterializer;
using gm_synth::sf2::sf2PanChannelGainQ15;

NativeResolvedZone baseZone() {
    NativeResolvedZone zone{};
    zone.bank = 0;
    zone.program = 0;
    zone.sample_id = 1;
    zone.key_low = 0;
    zone.key_high = 127;
    zone.velocity_low = 0;
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

}  // namespace

int main() {
    // Negative initialAttenuation is legal before the final SF2 summing-node
    // clamp. It may cancel velocity/controller attenuation, but the final gain
    // cannot exceed unity. Preserve the historical diagnostic bit without
    // treating this exact runtime behavior as a Preview-only feature.
    NativeResolvedZone native = baseZone();
    native.generators.set(48, -100);

    PlaybackZone output{};
    const auto result = Sf2PlaybackMaterializer{}.materialize(native, &output);
    assert(result.status == PlaybackMaterializationStatus::Qualified);
    assert(result.hasIssue(PlaybackIssueGainAboveUnityClamped));
    assert(output.note_on_gain.enabled);
    assert(output.note_on_gain.initial_attenuation_cb == -100);
    assert(output.gain_left_q15 == sf2PanChannelGainQ15(0, true));
    assert(output.gain_right_q15 == sf2PanChannelGainQ15(0, false));

    // The same final-unity rule applies to a negative sustainVolEnv amount.
    // Keep the diagnostic for observability, but do not downgrade an otherwise
    // representable zone.
    native = baseZone();
    native.generators.set(37, -100);
    const auto sustain = Sf2PlaybackMaterializer{}.materialize(native, &output);
    assert(sustain.status == PlaybackMaterializationStatus::Qualified);
    assert(sustain.hasIssue(PlaybackIssueGainAboveUnityClamped));
    assert(output.envelope.sustain_q15 == gm_synth::voice::kQ15Unity);

    std::cout << "M7 gain clamp qualification checks passed\n";
    return 0;
}
