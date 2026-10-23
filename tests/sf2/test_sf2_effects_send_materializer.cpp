#include "sf2/Sf2PlaybackMaterializer.h"
#include "sample/PublishedSampleDirectory.h"
#include "voice/AudioBlockRenderer.h"
#include "voice/VoiceRenderer.h"

#include <array>
#include <cassert>
#include <cstdint>
#include <cstdlib>
#include <iostream>

namespace {

using gm_synth::gm::PlaybackZone;
using gm_synth::sample::PublishedSampleDirectory;
using gm_synth::sample::ResidentRecord;
using gm_synth::sample::SampleDescriptor;
using gm_synth::sf2::NativeResolvedZone;
using gm_synth::sf2::PlaybackIssueEffectsSendPending;
using gm_synth::sf2::ResolvedZoneModulation;
using gm_synth::sf2::Sf2PlaybackMaterializer;
using gm_synth::voice::AudioBlockRenderer;
using gm_synth::voice::Voice;
using gm_synth::voice::VoicePool;
using gm_synth::voice::VoiceRuntimeControlUpdate;
using gm_synth::voice::VoiceStartParams;
using gm_synth::voice::kPhaseOneQ32;
using gm_synth::voice::kQ15Unity;

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
    assert(!result.hasIssue(PlaybackIssueEffectsSendPending));
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
    assert(!result.hasIssue(PlaybackIssueEffectsSendPending));
    assert(output.effects_send.reverb_sf2_units == 1000);
    assert(output.effects_send.chorus_sf2_units == 0);
}

struct RenderFixture {
    PublishedSampleDirectory directory;
    ResidentRecord record{};
    std::array<std::int16_t, 128> pcm{};

    explicit RenderFixture(std::int16_t value = 6000) {
        pcm.fill(value);
        record.bank_generation = directory.bankGeneration();
        record.sample_generation = 1;
        record.pcm = pcm.data();
        record.descriptor = SampleDescriptor{};
        record.descriptor.id = 0;
        record.descriptor.frames = pcm.size();
        record.descriptor.pcm_bytes = pcm.size() * sizeof(std::int16_t);
        record.descriptor.sample_rate = AudioBlockRenderer::kOutputSampleRate;
        assert(directory.publish(0, &record));
    }
};

VoiceStartParams renderParams(std::int16_t reverb, std::int16_t chorus) {
    VoiceStartParams out{};
    out.sample_id = 0;
    out.channel = 0;
    out.phase_increment_q32 = kPhaseOneQ32;
    out.gain_left_q15 = kQ15Unity;
    out.gain_right_q15 = kQ15Unity;
    out.reverb_send_units = reverb;
    out.chorus_send_units = chorus;
    out.envelope.sustain_q15 = kQ15Unity;
    out.envelope.release_frames = 8;
    return out;
}

std::uint64_t absEnergy(
    const std::array<std::int16_t, AudioBlockRenderer::kSamplesPerBlock>& block) {
    std::uint64_t result = 0;
    for (const std::int16_t sample : block) {
        result += static_cast<std::uint64_t>(std::abs(static_cast<int>(sample)));
    }
    return result;
}

std::uint64_t renderTailEnergy(
    std::int16_t reverb_send,
    std::int16_t chorus_send,
    bool add_zero_send_voice) {
    RenderFixture fixture;
    VoicePool pool(fixture.directory);
    AudioBlockRenderer renderer(pool);
    assert(pool.start(renderParams(reverb_send, chorus_send)));
    if (add_zero_send_voice) {
        assert(pool.start(renderParams(0, 0)));
    }

    std::array<std::int16_t, AudioBlockRenderer::kSamplesPerBlock> output{};
    // Two blocks consume the 128-frame source. Tail measurements begin only
    // after all dry source Voices have finished.
    renderer.render(output.data());
    renderer.render(output.data());
    assert(pool.activeVoiceCount() == 0);

    std::uint64_t energy = 0;
    for (int block = 0; block < 24; ++block) {
        output.fill(0);
        renderer.render(output.data());
        energy += absEnergy(output);
    }
    return energy;
}

void testChorusSendProducesDelayedWetTail() {
    assert(renderTailEnergy(0, 1000, false) > 0);
}

void testReverbSendProducesFeedbackTail() {
    assert(renderTailEnergy(1000, 0, false) > 0);
}

void testEffectsSendScalesWetEnergy() {
    const std::uint64_t half = renderTailEnergy(0, 500, false);
    const std::uint64_t full = renderTailEnergy(0, 1000, false);
    assert(half > 0);
    assert(full > half);
}

void testZeroSendVoiceDoesNotLeakIntoEffectsBus() {
    const std::uint64_t baseline = renderTailEnergy(1000, 1000, false);
    const std::uint64_t with_dry_only_layer = renderTailEnergy(1000, 1000, true);
    assert(baseline != 0);
    assert(with_dry_only_layer == baseline);
}

bool enableChorusOnActiveVoice(void*, Voice& voice) {
    VoiceRuntimeControlUpdate update{};
    update.phase_increment_q32 = voice.currentPhaseIncrementQ32();
    update.gain_left_q15 = voice.currentGainLeftQ15();
    update.gain_right_q15 = voice.currentGainRightQ15();
    update.reverb_send_units = voice.reverbSendUnits();
    update.chorus_send_units = 1000;
    return voice.applyRuntimeControlUpdate(update);
}

void testLiveEffectsSendUpdateIsConsumedByRenderer() {
    RenderFixture fixture;
    VoicePool pool(fixture.directory);
    AudioBlockRenderer renderer(pool);
    assert(pool.start(renderParams(0, 0)));

    std::array<std::int16_t, AudioBlockRenderer::kSamplesPerBlock> output{};
    renderer.render(output.data());
    assert(pool.activeVoiceCount() == 1);
    assert(pool.updateActiveVoices(0, enableChorusOnActiveVoice, nullptr) == 1);
    renderer.render(output.data());
    assert(pool.activeVoiceCount() == 0);

    std::uint64_t tail_energy = 0;
    for (int block = 0; block < 16; ++block) {
        output.fill(0);
        renderer.render(output.data());
        tail_energy += absEnergy(output);
    }
    assert(tail_energy > 0);
}

}  // namespace

int main() {
    testStaticGeneratorsAndResolvedControllerAmountsArePreserved();
    testStaticEffectsSendGeneratorsClampToSf2Range();
    testChorusSendProducesDelayedWetTail();
    testReverbSendProducesFeedbackTail();
    testEffectsSendScalesWetEnergy();
    testZeroSendVoiceDoesNotLeakIntoEffectsBus();
    testLiveEffectsSendUpdateIsConsumedByRenderer();
    std::cout << "SF2 effects-send materializer/runtime checks passed\n";
    return 0;
}
