#include "../../firmware/voice/VoiceRenderer.h"

#include <array>
#include <cassert>
#include <cstdint>
#include <iostream>

namespace {

using gm_synth::sample::PublishedSampleDirectory;
using gm_synth::sample::ResidentRecord;
using gm_synth::sample::SampleDescriptor;
using gm_synth::voice::LoopMode;
using gm_synth::voice::VoicePool;
using gm_synth::voice::VoiceStartParams;
using gm_synth::voice::kPhaseOneQ32;
using gm_synth::voice::kQ15Unity;

std::int32_t referenceScaleSend(std::int32_t sample, std::int32_t units) {
    constexpr std::int32_t kUnits = 1000;
    if (sample == 0 || units == 0) {
        return 0;
    }
    const std::int64_t product = static_cast<std::int64_t>(sample) * units;
    return product >= 0
        ? static_cast<std::int32_t>((product + kUnits / 2) / kUnits)
        : -static_cast<std::int32_t>(((-product) + kUnits / 2) / kUnits);
}

struct Fixture {
    PublishedSampleDirectory directory;
    ResidentRecord record{};
    std::array<std::int16_t, 8> pcm{
        -32768, -32767, -3, -1, 1, 3, 32766, 32767};

    Fixture() {
        record.bank_generation = directory.bankGeneration();
        record.sample_generation = 1;
        record.pcm = pcm.data();
        record.descriptor = SampleDescriptor{};
        record.descriptor.id = 0;
        record.descriptor.frames = pcm.size();
        record.descriptor.pcm_bytes = pcm.size() * sizeof(std::int16_t);
        record.descriptor.sample_rate = 44100;
        record.descriptor.loop_start = 0;
        record.descriptor.loop_end = pcm.size();
        assert(directory.publish(0, &record));
    }
};

VoiceStartParams paramsForSends(std::int16_t reverb, std::int16_t chorus) {
    VoiceStartParams params{};
    params.sample_id = 0;
    params.channel = 0;
    params.key = 60;
    params.velocity = 127;
    params.phase_increment_q32 = kPhaseOneQ32;
    params.loop_mode = LoopMode::Continuous;
    params.gain_left_q15 = kQ15Unity;
    params.gain_right_q15 = kQ15Unity;
    params.envelope.sustain_q15 = kQ15Unity;
    params.reverb_send_units = reverb;
    params.chorus_send_units = chorus;
    return params;
}

void testEqualHalfSendsReuseExactScale() {
    Fixture fixture;
    VoicePool voices(fixture.directory);
    assert(voices.start(paramsForSends(500, 500)));

    constexpr std::size_t kFrames = 16;
    std::array<std::int32_t, kFrames * 2> mix{};
    std::array<std::int32_t, kFrames * 2> reverb{};
    std::array<std::int32_t, kFrames * 2> chorus{};
    voices.renderAddWithEffects(mix.data(), reverb.data(), chorus.data(), kFrames);

    for (std::size_t i = 0; i < mix.size(); ++i) {
        assert(reverb[i] == referenceScaleSend(mix[i], 500));
        assert(chorus[i] == reverb[i]);
    }
    voices.allSoundOff();
}

void testHalfAndUnitySpecializationsMatchReference() {
    Fixture fixture;
    VoicePool voices(fixture.directory);
    assert(voices.start(paramsForSends(1000, 500)));

    constexpr std::size_t kFrames = 16;
    std::array<std::int32_t, kFrames * 2> mix{};
    std::array<std::int32_t, kFrames * 2> reverb{};
    std::array<std::int32_t, kFrames * 2> chorus{};
    voices.renderAddWithEffects(mix.data(), reverb.data(), chorus.data(), kFrames);

    for (std::size_t i = 0; i < mix.size(); ++i) {
        assert(reverb[i] == referenceScaleSend(mix[i], 1000));
        assert(chorus[i] == referenceScaleSend(mix[i], 500));
    }
    voices.allSoundOff();
}

void testGenericUnequalSendsRemainExact() {
    Fixture fixture;
    VoicePool voices(fixture.directory);
    assert(voices.start(paramsForSends(333, 777)));

    constexpr std::size_t kFrames = 16;
    std::array<std::int32_t, kFrames * 2> mix{};
    std::array<std::int32_t, kFrames * 2> reverb{};
    std::array<std::int32_t, kFrames * 2> chorus{};
    voices.renderAddWithEffects(mix.data(), reverb.data(), chorus.data(), kFrames);

    for (std::size_t i = 0; i < mix.size(); ++i) {
        assert(reverb[i] == referenceScaleSend(mix[i], 333));
        assert(chorus[i] == referenceScaleSend(mix[i], 777));
    }
    voices.allSoundOff();
}

}  // namespace

int main() {
    testEqualHalfSendsReuseExactScale();
    testHalfAndUnitySpecializationsMatchReference();
    testGenericUnequalSendsRemainExact();
    std::cout << "Effects send scaling checks passed\n";
    return 0;
}
