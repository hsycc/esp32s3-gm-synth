#include "../../firmware/voice/VoiceRenderer.h"

#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <iostream>

namespace {

using gm_synth::sample::PublishedSampleDirectory;
using gm_synth::sample::ResidentRecord;
using gm_synth::sample::SampleDescriptor;
using gm_synth::voice::LoopMode;
using gm_synth::voice::Voice;
using gm_synth::voice::VoiceRenderResult;
using gm_synth::voice::VoiceStartParams;
using gm_synth::voice::kPhaseOneQ32;

struct Fixture {
    PublishedSampleDirectory directory;
    ResidentRecord record{};

    bool publish(const std::int16_t* pcm, std::uint32_t frames) {
        record.bank_generation = directory.bankGeneration();
        record.sample_generation = 1;
        record.pcm = pcm;
        record.descriptor = SampleDescriptor{};
        record.descriptor.id = 0;
        record.descriptor.frames = frames;
        record.descriptor.pcm_bytes = frames * sizeof(std::int16_t);
        record.descriptor.sample_rate = 44100;
        record.descriptor.loop_start = 1;
        record.descriptor.loop_end = frames - 1u;
        return directory.publish(0, &record);
    }
};

VoiceStartParams makeParams(
    std::uint32_t release_frames,
    std::int32_t sustain_q15,
    std::uint32_t attack_frames = 0) {
    VoiceStartParams params{};
    params.sample_id = 0;
    params.channel = 0;
    params.key = 60;
    params.velocity = 100;
    params.phase_increment_q32 = kPhaseOneQ32 + kPhaseOneQ32 / 5u;
    params.loop_mode = LoopMode::Continuous;
    params.gain_left_q15 = 25123;
    params.gain_right_q15 = 17321;
    params.envelope.attack_frames = attack_frames;
    params.envelope.hold_frames = 0;
    params.envelope.decay_frames = 0;
    params.envelope.sustain_q15 = sustain_q15;
    params.envelope.release_frames = release_frames;
    return params;
}

void assertSameState(
    const Voice& generic,
    const Voice& realtime,
    const VoiceRenderResult& generic_result,
    const VoiceRenderResult& realtime_result) {
    assert(generic_result.frames_rendered == realtime_result.frames_rendered);
    assert(generic_result.finished == realtime_result.finished);
    assert(generic.active() == realtime.active());
    assert(generic.phaseQ32() == realtime.phaseQ32());
    assert(generic.envelopeLevelQ15() == realtime.envelopeLevelQ15());
    assert(generic.currentPhaseIncrementQ32() == realtime.currentPhaseIncrementQ32());
    assert(generic.currentGainLeftQ15() == realtime.currentGainLeftQ15());
    assert(generic.currentGainRightQ15() == realtime.currentGainRightQ15());
}

void renderAndCompare(
    Voice& generic,
    Voice& realtime,
    std::size_t frames) {
    assert(frames <= 64u);
    std::array<std::int32_t, 64u * 2u> generic_mix{};
    std::array<std::int32_t, 64u * 2u> realtime_mix{};
    const VoiceRenderResult generic_result = generic.renderAdd(generic_mix.data(), frames);
    const VoiceRenderResult realtime_result = realtime.renderAddRealtime(realtime_mix.data(), frames);
    for (std::size_t index = 0; index < frames * 2u; ++index) {
        assert(generic_mix[index] == realtime_mix[index]);
    }
    assertSameState(generic, realtime, generic_result, realtime_result);
}

void runReleaseCase(
    std::uint32_t release_frames,
    std::int32_t sustain_q15,
    std::uint32_t attack_frames,
    std::size_t pre_release_frames) {
    constexpr std::array<std::int16_t, 17> pcm{
        -14000, -11000, -8000, -5000, -2000, 1000, 4000, 7000, 10000,
        13000, 9000, 5000, 1000, -3000, -7000, -11000, -15000};

    Fixture generic_fixture;
    Fixture realtime_fixture;
    assert(generic_fixture.publish(pcm.data(), pcm.size()));
    assert(realtime_fixture.publish(pcm.data(), pcm.size()));

    Voice generic;
    Voice realtime;
    const VoiceStartParams params = makeParams(
        release_frames,
        sustain_q15,
        attack_frames);
    assert(generic.start(generic_fixture.directory, params));
    assert(realtime.start(realtime_fixture.directory, params));

    while (pre_release_frames != 0u) {
        const std::size_t chunk = pre_release_frames > 64u ? 64u : pre_release_frames;
        renderAndCompare(generic, realtime, chunk);
        pre_release_frames -= chunk;
    }

    generic.noteOff();
    realtime.noteOff();

    constexpr std::array<std::size_t, 7> chunks{64, 1, 7, 31, 3, 63, 17};
    std::size_t iteration = 0;
    while (generic.active() || realtime.active()) {
        assert(iteration < 20000u);
        renderAndCompare(generic, realtime, chunks[iteration % chunks.size()]);
        ++iteration;
    }

    assert(!generic.active());
    assert(!realtime.active());
    assert(generic_fixture.directory.stats().live_voice_refs == 0);
    assert(realtime_fixture.directory.stats().live_voice_refs == 0);
}

void testShortAndBoundaryReleaseCases() {
    runReleaseCase(1, 32767, 0, 0);
    runReleaseCase(2, 23000, 0, 0);
    runReleaseCase(9, 23000, 0, 0);
    runReleaseCase(64, 32767, 0, 0);
    runReleaseCase(65, 17000, 0, 0);
}

void testPhysicalStressShapeReleaseIsBitExactAcrossSegments() {
    // PhysicalMusicStress uses a 4410-frame (100 ms) release. This is the exact
    // shape that produced ~2.1 ms 16-Voice release blocks on ESP32-S3 before
    // the realtime release ramp specialization.
    runReleaseCase(4410, 32767, 0, 0);
}

void testPartialAttackReleaseStartIsBitExact() {
    // NoteOff during attack verifies that a non-unity release_start_level_q15
    // still matches the canonical renderer exactly.
    runReleaseCase(257, 21000, 97, 23);
}

void testLargeCommonFastRangeReleaseIsBitExact() {
    // 100000 * Q15 unity remains below UINT32_MAX, covering the upper portion
    // of the division-free common realtime ramp initialization range.
    runReleaseCase(100000, 32767, 0, 0);
}

void testVeryLongReleaseFallsBackBitExactly() {
    // Releases whose start*remaining product exceeds UINT32_MAX deliberately
    // stay on the canonical path rather than trading correctness for speed.
    runReleaseCase(160000, 32767, 0, 0);
}

}  // namespace

int main() {
    testShortAndBoundaryReleaseCases();
    testPhysicalStressShapeReleaseIsBitExactAcrossSegments();
    testPartialAttackReleaseStartIsBitExact();
    testLargeCommonFastRangeReleaseIsBitExact();
    testVeryLongReleaseFallsBackBitExactly();
    std::cout << "Voice realtime release fast-path checks passed\n";
    return 0;
}
