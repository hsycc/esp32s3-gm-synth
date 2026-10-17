#include "../../firmware/voice/VoiceRenderer.h"

#include <array>
#include <cassert>
#include <cstdint>
#include <iostream>
#include <limits>

namespace {

using gm_synth::sample::PublishedSampleDirectory;
using gm_synth::sample::ResidentRecord;
using gm_synth::sample::SampleDescriptor;
using gm_synth::voice::LoopMode;
using gm_synth::voice::Voice;
using gm_synth::voice::VoiceStartParams;
using gm_synth::voice::kPhaseOneQ32;
using gm_synth::voice::kQ15Unity;

struct Fixture {
    PublishedSampleDirectory directory;
    ResidentRecord record{};

    bool publish(const std::int16_t* pcm, std::uint32_t frames,
                 std::uint32_t loop_start, std::uint32_t loop_end) {
        record.bank_generation = directory.bankGeneration();
        record.sample_generation = 1;
        record.pcm = pcm;
        record.descriptor = SampleDescriptor{};
        record.descriptor.id = 0;
        record.descriptor.frames = frames;
        record.descriptor.pcm_bytes = frames * sizeof(std::int16_t);
        record.descriptor.sample_rate = 44100;
        record.descriptor.loop_start = loop_start;
        record.descriptor.loop_end = loop_end;
        return directory.publish(0, &record);
    }
};

VoiceStartParams steadyParams() {
    VoiceStartParams params{};
    params.sample_id = 0;
    params.channel = 1;
    params.key = 60;
    params.velocity = 100;
    params.phase_increment_q32 = kPhaseOneQ32 + kPhaseOneQ32 / 3u;
    params.loop_mode = LoopMode::Continuous;
    params.gain_left_q15 = 25000;
    params.gain_right_q15 = 17000;
    params.envelope.attack_frames = 0;
    params.envelope.hold_frames = 0;
    params.envelope.decay_frames = 0;
    params.envelope.sustain_q15 = 23000;
    params.envelope.release_frames = 8;
    return params;
}

void assertSameRenderState(
    const Voice& generic,
    const Voice& realtime,
    const gm_synth::voice::VoiceRenderResult& generic_result,
    const gm_synth::voice::VoiceRenderResult& realtime_result) {
    assert(generic_result.frames_rendered == realtime_result.frames_rendered);
    assert(generic_result.finished == realtime_result.finished);
    assert(generic.phaseQ32() == realtime.phaseQ32());
    assert(generic.envelopeLevelQ15() == realtime.envelopeLevelQ15());
    assert(generic.currentGainLeftQ15() == realtime.currentGainLeftQ15());
    assert(generic.currentGainRightQ15() == realtime.currentGainRightQ15());
}

void testSteadySustainFastPathIsBitExact() {
    constexpr std::array<std::int16_t, 9> pcm{
        1200, -4000, 8000, -12000, 16000, -8000, 4000, -2000, 6000};

    Fixture generic_fixture;
    Fixture realtime_fixture;
    assert(generic_fixture.publish(pcm.data(), pcm.size(), 1, 8));
    assert(realtime_fixture.publish(pcm.data(), pcm.size(), 1, 8));

    Voice generic;
    Voice realtime;
    const VoiceStartParams params = steadyParams();
    assert(generic.start(generic_fixture.directory, params));
    assert(realtime.start(realtime_fixture.directory, params));

    constexpr std::size_t kFrames = 48;
    std::array<std::int32_t, kFrames * 2> generic_mix{};
    std::array<std::int32_t, kFrames * 2> realtime_mix{};

    const auto generic_result = generic.renderAdd(generic_mix.data(), kFrames);
    const auto realtime_result = realtime.renderAddRealtime(realtime_mix.data(), kFrames);

    assert(generic_mix == realtime_mix);
    assertSameRenderState(generic, realtime, generic_result, realtime_result);
    assert(generic_fixture.directory.stats().live_voice_refs == 1);
    assert(realtime_fixture.directory.stats().live_voice_refs == 1);

    generic.reset();
    realtime.reset();
    assert(generic_fixture.directory.stats().live_voice_refs == 0);
    assert(realtime_fixture.directory.stats().live_voice_refs == 0);
}

void testMultiLoopIncrementFallbackIsBitExact() {
    constexpr std::array<std::int16_t, 10> pcm{
        900, -1800, 3600, -7200, 14400, -11000, 7000, -3500, 1750, -800};

    Fixture generic_fixture;
    Fixture realtime_fixture;
    assert(generic_fixture.publish(pcm.data(), pcm.size(), 1, 8));
    assert(realtime_fixture.publish(pcm.data(), pcm.size(), 1, 8));

    VoiceStartParams params = steadyParams();
    // Seven-frame loop, but advance by more than two loop lengths per output
    // frame. This must take the modulo fallback rather than the common
    // single-wrap path and still match the generic reference exactly.
    params.phase_increment_q32 = 15u * kPhaseOneQ32 + kPhaseOneQ32 / 4u;

    Voice generic;
    Voice realtime;
    assert(generic.start(generic_fixture.directory, params));
    assert(realtime.start(realtime_fixture.directory, params));

    constexpr std::size_t kFrames = 32;
    std::array<std::int32_t, kFrames * 2> generic_mix{};
    std::array<std::int32_t, kFrames * 2> realtime_mix{};
    const auto generic_result = generic.renderAdd(generic_mix.data(), kFrames);
    const auto realtime_result = realtime.renderAddRealtime(realtime_mix.data(), kFrames);

    assert(generic_mix == realtime_mix);
    assertSameRenderState(generic, realtime, generic_result, realtime_result);

    generic.reset();
    realtime.reset();
}

void testQ15GainInt32ExtremesAreBitExact() {
    constexpr std::array<std::int16_t, 8> pcm{
        std::numeric_limits<std::int16_t>::min(),
        std::numeric_limits<std::int16_t>::max(),
        -32767,
        32766,
        -1,
        1,
        -16384,
        16384,
    };

    Fixture generic_fixture;
    Fixture realtime_fixture;
    assert(generic_fixture.publish(pcm.data(), pcm.size(), 0, 8));
    assert(realtime_fixture.publish(pcm.data(), pcm.size(), 0, 8));

    VoiceStartParams params = steadyParams();
    params.phase_increment_q32 = kPhaseOneQ32;
    params.gain_left_q15 = kQ15Unity - 1;
    params.gain_right_q15 = kQ15Unity / 2;
    params.envelope.sustain_q15 = kQ15Unity;

    Voice generic;
    Voice realtime;
    assert(generic.start(generic_fixture.directory, params));
    assert(realtime.start(realtime_fixture.directory, params));

    constexpr std::size_t kFrames = 32;
    std::array<std::int32_t, kFrames * 2> generic_mix{};
    std::array<std::int32_t, kFrames * 2> realtime_mix{};
    const auto generic_result = generic.renderAdd(generic_mix.data(), kFrames);
    const auto realtime_result = realtime.renderAddRealtime(realtime_mix.data(), kFrames);

    assert(generic_mix == realtime_mix);
    assertSameRenderState(generic, realtime, generic_result, realtime_result);

    generic.reset();
    realtime.reset();
}

void testSimpleAttackDecayFastPathIsBitExact() {
    constexpr std::array<std::int16_t, 8> pcm{
        1000, -2000, 4000, -8000, 12000, -6000, 3000, -1500};

    Fixture generic_fixture;
    Fixture realtime_fixture;
    assert(generic_fixture.publish(pcm.data(), pcm.size(), 1, 7));
    assert(realtime_fixture.publish(pcm.data(), pcm.size(), 1, 7));

    VoiceStartParams params = steadyParams();
    params.envelope.attack_frames = 5;
    params.envelope.decay_frames = 7;
    params.envelope.sustain_q15 = 19000;

    Voice generic;
    Voice realtime;
    assert(generic.start(generic_fixture.directory, params));
    assert(realtime.start(realtime_fixture.directory, params));

    constexpr std::size_t kFrames = 24;
    std::array<std::int32_t, kFrames * 2> generic_mix{};
    std::array<std::int32_t, kFrames * 2> realtime_mix{};
    const auto generic_result = generic.renderAdd(generic_mix.data(), kFrames);
    const auto realtime_result = realtime.renderAddRealtime(realtime_mix.data(), kFrames);

    assert(generic_mix == realtime_mix);
    assertSameRenderState(generic, realtime, generic_result, realtime_result);

    generic.reset();
    realtime.reset();
}

void testSimpleReleaseFastPathIsBitExact() {
    constexpr std::array<std::int16_t, 9> pcm{
        -9000, -4500, 0, 4500, 9000, 4500, 0, -4500, -9000};

    Fixture generic_fixture;
    Fixture realtime_fixture;
    assert(generic_fixture.publish(pcm.data(), pcm.size(), 1, 8));
    assert(realtime_fixture.publish(pcm.data(), pcm.size(), 1, 8));

    VoiceStartParams params = steadyParams();
    params.envelope.release_frames = 9;

    Voice generic;
    Voice realtime;
    assert(generic.start(generic_fixture.directory, params));
    assert(realtime.start(realtime_fixture.directory, params));
    generic.noteOff();
    realtime.noteOff();

    constexpr std::size_t kFrames = 16;
    std::array<std::int32_t, kFrames * 2> generic_mix{};
    std::array<std::int32_t, kFrames * 2> realtime_mix{};
    const auto generic_result = generic.renderAdd(generic_mix.data(), kFrames);
    const auto realtime_result = realtime.renderAddRealtime(realtime_mix.data(), kFrames);

    assert(generic_mix == realtime_mix);
    assertSameRenderState(generic, realtime, generic_result, realtime_result);
    assert(generic_result.finished);
    assert(generic_fixture.directory.stats().live_voice_refs == 0);
    assert(realtime_fixture.directory.stats().live_voice_refs == 0);
}

void testUntilReleaseModeFallsBackBitExact() {
    constexpr std::array<std::int16_t, 9> pcm{
        700, -1400, 2800, -5600, 11200, -5600, 2800, -1400, 700};

    Fixture generic_fixture;
    Fixture realtime_fixture;
    assert(generic_fixture.publish(pcm.data(), pcm.size(), 1, 8));
    assert(realtime_fixture.publish(pcm.data(), pcm.size(), 1, 8));

    VoiceStartParams params = steadyParams();
    params.loop_mode = LoopMode::UntilRelease;

    Voice generic;
    Voice realtime;
    assert(generic.start(generic_fixture.directory, params));
    assert(realtime.start(realtime_fixture.directory, params));

    constexpr std::size_t kFrames = 24;
    std::array<std::int32_t, kFrames * 2> generic_mix{};
    std::array<std::int32_t, kFrames * 2> realtime_mix{};
    const auto generic_result = generic.renderAdd(generic_mix.data(), kFrames);
    const auto realtime_result = realtime.renderAddRealtime(realtime_mix.data(), kFrames);

    assert(generic_mix == realtime_mix);
    assertSameRenderState(generic, realtime, generic_result, realtime_result);

    generic.reset();
    realtime.reset();
}

}  // namespace

int main() {
    testSteadySustainFastPathIsBitExact();
    testMultiLoopIncrementFallbackIsBitExact();
    testQ15GainInt32ExtremesAreBitExact();
    testSimpleAttackDecayFastPathIsBitExact();
    testSimpleReleaseFastPathIsBitExact();
    testUntilReleaseModeFallsBackBitExact();
    std::cout << "Voice realtime fast-path checks passed\n";
    return 0;
}
