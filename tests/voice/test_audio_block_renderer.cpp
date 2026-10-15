#include "../../firmware/voice/AudioBlockRenderer.h"

#include <array>
#include <cassert>
#include <cstdint>
#include <iostream>
#include <limits>

namespace {

using gm_synth::sample::PublishedSampleDirectory;
using gm_synth::sample::ResidentRecord;
using gm_synth::sample::SampleDescriptor;
using gm_synth::voice::AudioBlockRenderer;
using gm_synth::voice::VoicePool;
using gm_synth::voice::VoiceStartParams;
using gm_synth::voice::kPhaseOneQ32;
using gm_synth::voice::kQ15Unity;

struct Fixture {
    PublishedSampleDirectory directory;
    ResidentRecord record{};
    std::array<std::int16_t, AudioBlockRenderer::kFramesPerBlock> pcm{};

    void publish(std::int16_t value) {
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

VoiceStartParams params() {
    VoiceStartParams out{};
    out.sample_id = 0;
    out.phase_increment_q32 = kPhaseOneQ32;
    out.gain_left_q15 = kQ15Unity;
    out.gain_right_q15 = kQ15Unity;
    out.envelope.sustain_q15 = kQ15Unity;
    out.envelope.release_frames = 8;
    return out;
}

void testSilenceIsDeterministic() {
    Fixture fixture;
    fixture.publish(1234);
    VoicePool pool(fixture.directory);
    AudioBlockRenderer renderer(pool);

    std::array<std::int16_t, AudioBlockRenderer::kSamplesPerBlock> output{};
    output.fill(123);
    renderer.render(output.data());

    for (const auto sample : output) {
        assert(sample == 0);
    }
    assert(renderer.stats().blocks_rendered == 1);
    assert(renderer.stats().output_frames == AudioBlockRenderer::kFramesPerBlock);
    assert(renderer.stats().clipped_samples == 0);
}

void testOneVoiceProducesExactBlock() {
    Fixture fixture;
    fixture.publish(12000);
    VoicePool pool(fixture.directory);
    AudioBlockRenderer renderer(pool);
    assert(pool.start(params()));

    std::array<std::int16_t, AudioBlockRenderer::kSamplesPerBlock> output{};
    renderer.render(output.data());

    for (const auto sample : output) {
        assert(sample == 12000);
    }
    assert(pool.activeVoiceCount() == 0);
    assert(renderer.stats().peak_abs_accumulator == 12000);
    assert(renderer.stats().clipped_samples == 0);
    assert(pool.stats().rendered_voice_frames == AudioBlockRenderer::kFramesPerBlock);
}

void testPositiveMixSaturatesAtOutputBoundary() {
    Fixture fixture;
    fixture.publish(20000);
    VoicePool pool(fixture.directory);
    AudioBlockRenderer renderer(pool);
    assert(pool.start(params()));
    assert(pool.start(params()));

    std::array<std::int16_t, AudioBlockRenderer::kSamplesPerBlock> output{};
    renderer.render(output.data());

    for (const auto sample : output) {
        assert(sample == std::numeric_limits<std::int16_t>::max());
    }
    assert(renderer.stats().peak_abs_accumulator == 40000);
    assert(renderer.stats().clipped_samples == AudioBlockRenderer::kSamplesPerBlock);
}

void testNegativeMixSaturatesAtOutputBoundary() {
    Fixture fixture;
    fixture.publish(-20000);
    VoicePool pool(fixture.directory);
    AudioBlockRenderer renderer(pool);
    assert(pool.start(params()));
    assert(pool.start(params()));

    std::array<std::int16_t, AudioBlockRenderer::kSamplesPerBlock> output{};
    renderer.render(output.data());

    for (const auto sample : output) {
        assert(sample == std::numeric_limits<std::int16_t>::min());
    }
    assert(renderer.stats().peak_abs_accumulator == 40000);
    assert(renderer.stats().clipped_samples == AudioBlockRenderer::kSamplesPerBlock);
}

void testNullOutputDoesNotAdvanceBlockStats() {
    Fixture fixture;
    fixture.publish(1000);
    VoicePool pool(fixture.directory);
    AudioBlockRenderer renderer(pool);
    renderer.render(nullptr);
    assert(renderer.stats().blocks_rendered == 0);
    assert(renderer.stats().output_frames == 0);
}

}  // namespace

int main() {
    testSilenceIsDeterministic();
    testOneVoiceProducesExactBlock();
    testPositiveMixSaturatesAtOutputBoundary();
    testNegativeMixSaturatesAtOutputBoundary();
    testNullOutputDoesNotAdvanceBlockStats();
    std::cout << "audio block renderer tests passed\n";
    return 0;
}
