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
using gm_synth::voice::VoiceStealConfig;
using gm_synth::voice::kPhaseOneQ32;
using gm_synth::voice::kQ15Unity;

struct PublishedFixture {
    PublishedSampleDirectory directory;
    ResidentRecord record{};

    bool publish(
        std::uint32_t sample_id,
        const std::int16_t* pcm,
        std::uint32_t frames,
        std::uint32_t loop_start = 0,
        std::uint32_t loop_end = 0) {
        record.bank_generation = directory.bankGeneration();
        record.sample_generation = 1;
        record.pcm = pcm;
        record.descriptor = SampleDescriptor{};
        record.descriptor.id = sample_id;
        record.descriptor.frames = frames;
        record.descriptor.pcm_bytes = frames * sizeof(std::int16_t);
        record.descriptor.sample_rate = 44100;
        record.descriptor.loop_start = loop_start;
        record.descriptor.loop_end = loop_end;
        return directory.publish(sample_id, &record);
    }
};

VoiceStartParams basicParams(std::uint32_t sample_id = 0) {
    VoiceStartParams params{};
    params.sample_id = sample_id;
    params.channel = 2;
    params.key = 64;
    params.velocity = 100;
    params.phase_increment_q32 = kPhaseOneQ32;
    params.gain_left_q15 = kQ15Unity;
    params.gain_right_q15 = kQ15Unity;
    params.envelope.attack_frames = 0;
    params.envelope.decay_frames = 0;
    params.envelope.sustain_q15 = kQ15Unity;
    params.envelope.release_frames = 8;
    return params;
}

VoiceStartParams loopingParams(std::uint16_t exclusive_class = 0, std::uint32_t note_instance_id = 0) {
    auto params = basicParams();
    params.loop_mode = LoopMode::Continuous;
    params.exclusive_class = exclusive_class;
    params.note_instance_id = note_instance_id;
    return params;
}

VoiceStealConfig fastReleaseConfig(
    std::uint32_t fade_frames,
    std::size_t max_tail_voices,
    bool stealing_enabled = false) {
    VoiceStealConfig config{};
    config.enabled = stealing_enabled;
    config.fade_frames = fade_frames;
    config.max_tail_voices = max_tail_voices;
    return config;
}

void testOneShotAndVoiceRefLifetime() {
    constexpr std::array<std::int16_t, 4> pcm{1000, 2000, 3000, 4000};
    PublishedFixture fixture;
    assert(fixture.publish(0, pcm.data(), pcm.size()));

    VoicePool pool(fixture.directory);
    assert(pool.start(basicParams()));
    assert(pool.activeVoiceCount() == 1);
    assert(fixture.directory.stats().live_voice_refs == 1);

    std::array<std::int32_t, 12> out{};
    pool.renderAdd(out.data(), 6);

    assert(out[0] == 1000 && out[1] == 1000);
    assert(out[2] == 2000 && out[3] == 2000);
    assert(out[4] == 3000 && out[5] == 3000);
    assert(out[6] == 4000 && out[7] == 4000);
    assert(out[8] == 0 && out[9] == 0 && out[10] == 0 && out[11] == 0);
    assert(pool.activeVoiceCount() == 0);
    assert(pool.stats().rendered_voice_frames == 4);
    assert(pool.stats().voices_finished == 1);
    assert(fixture.directory.stats().live_voice_refs == 0);
}

void testSignedPcmUsesSymmetricQ15Rounding() {
    constexpr std::array<std::int16_t, 4> pcm{-1000, -2000, 1000, 2000};
    PublishedFixture fixture;
    assert(fixture.publish(0, pcm.data(), pcm.size()));

    VoicePool pool(fixture.directory);
    assert(pool.start(basicParams()));

    std::array<std::int32_t, 8> out{};
    pool.renderAdd(out.data(), 4);
    assert(out[0] == -1000 && out[1] == -1000);
    assert(out[2] == -2000 && out[3] == -2000);
    assert(out[4] == 1000 && out[5] == 1000);
    assert(out[6] == 2000 && out[7] == 2000);
    assert(fixture.directory.stats().live_voice_refs == 0);
}

void testLinearInterpolationAtHalfRate() {
    constexpr std::array<std::int16_t, 4> pcm{0, 10000, 20000, 30000};
    PublishedFixture fixture;
    assert(fixture.publish(0, pcm.data(), pcm.size()));

    VoicePool pool(fixture.directory);
    auto params = basicParams();
    params.phase_increment_q32 = kPhaseOneQ32 / 2u;
    assert(pool.start(params));

    std::array<std::int32_t, 8> out{};
    pool.renderAdd(out.data(), 4);
    assert(out[0] == 0);
    assert(out[2] == 5000);
    assert(out[4] == 10000);
    assert(out[6] == 15000);
    pool.allSoundOff();
    assert(fixture.directory.stats().live_voice_refs == 0);
}

void testLinearInterpolationAcrossNegativeSlope() {
    constexpr std::array<std::int16_t, 3> pcm{10000, -10000, -20000};
    PublishedFixture fixture;
    assert(fixture.publish(0, pcm.data(), pcm.size()));

    VoicePool pool(fixture.directory);
    auto params = basicParams();
    params.phase_increment_q32 = kPhaseOneQ32 / 2u;
    assert(pool.start(params));

    std::array<std::int32_t, 6> out{};
    pool.renderAdd(out.data(), 3);
    assert(out[0] == 10000);
    assert(out[2] == 0);
    assert(out[4] == -10000);
    pool.allSoundOff();
}

void testContinuousLoopWrapsDeterministically() {
    constexpr std::array<std::int16_t, 4> pcm{0, 100, 200, 300};
    PublishedFixture fixture;
    assert(fixture.publish(0, pcm.data(), pcm.size(), 1, 3));

    VoicePool pool(fixture.directory);
    auto params = basicParams();
    params.loop_mode = LoopMode::Continuous;
    assert(pool.start(params));

    std::array<std::int32_t, 12> out{};
    pool.renderAdd(out.data(), 6);
    const std::array<std::int32_t, 6> expected{0, 100, 200, 100, 200, 100};
    for (std::size_t i = 0; i < expected.size(); ++i) {
        assert(out[i * 2u] == expected[i]);
        assert(out[i * 2u + 1u] == expected[i]);
    }
    assert(pool.activeVoiceCount() == 1);
    pool.allSoundOff();
}

void testUntilReleaseLeavesLoopAndPlaysTail() {
    constexpr std::array<std::int16_t, 4> pcm{0, 100, 200, 300};
    PublishedFixture fixture;
    assert(fixture.publish(0, pcm.data(), pcm.size(), 1, 3));

    VoicePool pool(fixture.directory);
    auto params = basicParams();
    params.loop_mode = LoopMode::UntilRelease;
    params.envelope.release_frames = 16;
    assert(pool.start(params));

    std::array<std::int32_t, 8> before{};
    pool.renderAdd(before.data(), 4);
    assert(before[0] == 0);
    assert(before[2] == 100);
    assert(before[4] == 200);
    assert(before[6] == 100);

    assert(pool.noteOff(params.channel, params.key) == 1);
    std::array<std::int32_t, 8> after{};
    pool.renderAdd(after.data(), 4);
    assert(after[0] > 0);
    assert(after[2] > after[0]);
    assert(after[4] == 0 && after[6] == 0);
    assert(pool.activeVoiceCount() == 0);
    assert(fixture.directory.stats().live_voice_refs == 0);
}

void testReleaseEnvelopeCompletesOnContinuousLoop() {
    constexpr std::array<std::int16_t, 4> pcm{1000, 1000, 1000, 1000};
    PublishedFixture fixture;
    assert(fixture.publish(0, pcm.data(), pcm.size(), 0, 4));

    VoicePool pool(fixture.directory);
    auto params = basicParams();
    params.loop_mode = LoopMode::Continuous;
    params.envelope.release_frames = 4;
    assert(pool.start(params));

    std::array<std::int32_t, 2> initial{};
    pool.renderAdd(initial.data(), 1);
    assert(initial[0] == 1000);
    assert(pool.noteOff(params.channel, params.key) == 1);

    std::array<std::int32_t, 12> release{};
    pool.renderAdd(release.data(), 6);
    assert(release[0] == 1000);
    assert(release[2] > release[4]);
    assert(release[4] > release[6]);
    assert(release[6] > 0);
    assert(release[8] == 0 && release[10] == 0);
    assert(pool.activeVoiceCount() == 0);
    assert(fixture.directory.stats().live_voice_refs == 0);
}

void testDuplicateSameNoteStacksAndNoteOffReleasesAllMatches() {
    constexpr std::array<std::int16_t, 16> pcm{
        1000, 1000, 1000, 1000, 1000, 1000, 1000, 1000,
        1000, 1000, 1000, 1000, 1000, 1000, 1000, 1000};
    PublishedFixture fixture;
    assert(fixture.publish(0, pcm.data(), pcm.size(), 0, pcm.size()));

    VoicePool pool(fixture.directory);
    auto params = loopingParams();
    params.envelope.release_frames = 4;
    assert(pool.start(params));
    assert(pool.start(params));
    assert(pool.activeVoiceCount() == 2);
    assert(pool.stats().exclusive_choke_attempts == 0);

    assert(pool.noteOff(params.channel, params.key) == 2);
    std::array<std::int32_t, 8> release{};
    pool.renderAdd(release.data(), 4);
    assert(pool.activeVoiceCount() == 0);
    assert(pool.stats().note_off_matches == 2);
    assert(fixture.directory.stats().live_voice_refs == 0);
}

void testExclusiveClassPreservesLayersFromSameNoteInstance() {
    constexpr std::array<std::int16_t, 16> pcm{
        1000, 1000, 1000, 1000, 1000, 1000, 1000, 1000,
        1000, 1000, 1000, 1000, 1000, 1000, 1000, 1000};
    PublishedFixture fixture;
    assert(fixture.publish(0, pcm.data(), pcm.size(), 0, pcm.size()));

    VoicePool pool(fixture.directory);
    pool.setVoiceStealConfig(fastReleaseConfig(4, 2));

    auto first_layer = loopingParams(7, 100);
    auto second_layer = first_layer;
    second_layer.key = 65;
    assert(pool.start(first_layer));
    assert(pool.start(second_layer));
    assert(pool.activeVoiceCount() == 2);
    assert(pool.activeExclusiveTailCount() == 0);
    assert(pool.stats().exclusive_choke_attempts == 0);

    auto next_note = loopingParams(7, 101);
    next_note.key = 66;
    assert(pool.start(next_note));
    assert(pool.activeVoiceCount() == 3);
    assert(pool.activeExclusiveTailCount() == 2);
    assert(pool.activeFastReleaseTailCount() == 2);
    assert(pool.stats().exclusive_choke_attempts == 1);
    assert(pool.stats().exclusive_choke_successes == 1);
    assert(pool.stats().exclusive_voices_released == 2);
    assert(pool.stats().last_exclusive_class == 7);

    std::array<std::int32_t, 8> mix{};
    pool.renderAdd(mix.data(), 4);
    assert(pool.activeVoiceCount() == 1);
    assert(pool.activeExclusiveTailCount() == 0);
    assert(pool.activeFastReleaseTailCount() == 0);
    assert(fixture.directory.stats().live_voice_refs == 1);
    pool.allSoundOff();
}

void testExclusiveClassIsScopedToMidiChannel() {
    constexpr std::array<std::int16_t, 8> pcm{1000, 1000, 1000, 1000, 1000, 1000, 1000, 1000};
    PublishedFixture fixture;
    assert(fixture.publish(0, pcm.data(), pcm.size(), 0, pcm.size()));

    VoicePool pool(fixture.directory);
    auto channel_two = loopingParams(9, 1);
    auto channel_three = loopingParams(9, 2);
    channel_three.channel = 3;

    assert(pool.start(channel_two));
    assert(pool.start(channel_three));
    assert(pool.activeVoiceCount() == 2);
    assert(pool.activeExclusiveTailCount() == 0);
    assert(pool.stats().exclusive_choke_attempts == 0);
    pool.allSoundOff();
}

void testExclusiveReplacementAtAdmissionLimitDoesNotRequireStealing() {
    constexpr std::array<std::int16_t, 8> pcm{1000, 1000, 1000, 1000, 1000, 1000, 1000, 1000};
    PublishedFixture fixture;
    assert(fixture.publish(0, pcm.data(), pcm.size(), 0, pcm.size()));

    VoicePool pool(fixture.directory);
    pool.setAdmissionLimit(1);
    pool.setVoiceStealConfig(fastReleaseConfig(4, 1, false));

    assert(pool.start(loopingParams(12, 1)));
    assert(pool.start(loopingParams(12, 2)));
    assert(pool.activeVoiceCount() == 2);
    assert(pool.activeExclusiveTailCount() == 1);
    assert(pool.stats().steal_attempts == 0);
    assert(pool.stats().exclusive_choke_successes == 1);

    std::array<std::int32_t, 8> mix{};
    pool.renderAdd(mix.data(), 4);
    assert(pool.activeVoiceCount() == 1);
    assert(pool.activeExclusiveTailCount() == 0);
    pool.allSoundOff();
}

void testExclusiveRejectedIncomingNeverSacrificesExistingVoice() {
    constexpr std::array<std::int16_t, 8> pcm{1000, 1000, 1000, 1000, 1000, 1000, 1000, 1000};
    PublishedFixture fixture;
    assert(fixture.publish(0, pcm.data(), pcm.size(), 0, pcm.size()));

    VoicePool pool(fixture.directory);
    auto old_voice = loopingParams(15, 1);
    assert(pool.start(old_voice));
    assert(fixture.directory.stats().live_voice_refs == 1);

    auto missing = loopingParams(15, 2);
    missing.sample_id = 1;
    assert(!pool.start(missing));
    assert(pool.activeVoiceCount() == 1);
    assert(pool.activeExclusiveTailCount() == 0);
    assert(pool.stats().start_failures_rejected == 1);
    assert(pool.stats().exclusive_choke_successes == 0);
    assert(fixture.directory.stats().live_voice_refs == 1);
    pool.allSoundOff();
}

void testExclusiveTailBudgetRejectsWithoutPartialChoke() {
    constexpr std::array<std::int16_t, 8> pcm{1000, 1000, 1000, 1000, 1000, 1000, 1000, 1000};
    PublishedFixture fixture;
    assert(fixture.publish(0, pcm.data(), pcm.size(), 0, pcm.size()));

    VoicePool pool(fixture.directory);
    pool.setVoiceStealConfig(fastReleaseConfig(8, 1));
    assert(pool.start(loopingParams(20, 1)));
    assert(pool.start(loopingParams(20, 1)));
    assert(pool.activeVoiceCount() == 2);
    assert(pool.activeFastReleaseTailCount() == 0);

    assert(!pool.start(loopingParams(20, 2)));
    assert(pool.activeVoiceCount() == 2);
    assert(pool.activeFastReleaseTailCount() == 0);
    assert(pool.stats().exclusive_choke_attempts == 1);
    assert(pool.stats().exclusive_choke_failures_tail_budget == 1);
    assert(pool.stats().exclusive_choke_successes == 0);
    assert(fixture.directory.stats().live_voice_refs == 2);
    pool.allSoundOff();
}

void testExclusiveReplacementDoesNotFightLoweredAdmissionTarget() {
    constexpr std::array<std::int16_t, 8> pcm{1000, 1000, 1000, 1000, 1000, 1000, 1000, 1000};
    PublishedFixture fixture;
    assert(fixture.publish(0, pcm.data(), pcm.size(), 0, pcm.size()));

    VoicePool pool(fixture.directory);
    pool.setVoiceStealConfig(fastReleaseConfig(4, 2));
    auto exclusive = loopingParams(22, 1);
    assert(pool.start(exclusive));

    auto unrelated = loopingParams();
    unrelated.key = 70;
    assert(pool.start(unrelated));
    assert(pool.activeVoiceCount() == 2);

    pool.setAdmissionLimit(1);
    auto replacement = loopingParams(22, 2);
    assert(!pool.start(replacement));
    assert(pool.activeVoiceCount() == 2);
    assert(pool.activeFastReleaseTailCount() == 0);
    assert(pool.stats().exclusive_choke_failures_over_target == 1);
    assert(pool.stats().exclusive_choke_successes == 0);
    pool.allSoundOff();
}

void testHardVoiceCapAndRejectedStartAreBounded() {
    constexpr std::array<std::int16_t, 8> pcm{100, 100, 100, 100, 100, 100, 100, 100};
    PublishedFixture fixture;
    assert(fixture.publish(0, pcm.data(), pcm.size(), 0, pcm.size()));

    VoicePool pool(fixture.directory);
    auto missing = basicParams(1);
    assert(!pool.start(missing));
    assert(pool.stats().start_failures_rejected == 1);

    auto params = basicParams();
    params.loop_mode = LoopMode::Continuous;
    for (std::size_t i = 0; i < VoicePool::kHardVoiceCap; ++i) {
        assert(pool.start(params));
    }
    assert(pool.activeVoiceCount() == VoicePool::kHardVoiceCap);
    assert(fixture.directory.stats().live_voice_refs == VoicePool::kHardVoiceCap);
    assert(!pool.start(params));
    assert(pool.stats().start_failures_full == 1);

    pool.allSoundOff();
    assert(pool.activeVoiceCount() == 0);
    assert(fixture.directory.stats().live_voice_refs == 0);
}

void testInvalidLoopAndZeroPhaseRejectWithoutLeakingRef() {
    constexpr std::array<std::int16_t, 4> pcm{100, 200, 300, 400};
    PublishedFixture fixture;
    assert(fixture.publish(0, pcm.data(), pcm.size()));

    VoicePool pool(fixture.directory);
    auto invalid_loop = basicParams();
    invalid_loop.loop_mode = LoopMode::Continuous;
    assert(!pool.start(invalid_loop));
    assert(fixture.directory.stats().live_voice_refs == 0);

    auto zero_phase = basicParams();
    zero_phase.phase_increment_q32 = 0;
    assert(!pool.start(zero_phase));
    assert(fixture.directory.stats().live_voice_refs == 0);
    assert(pool.stats().start_failures_rejected == 2);
}

}  // namespace

int main() {
    testOneShotAndVoiceRefLifetime();
    testSignedPcmUsesSymmetricQ15Rounding();
    testLinearInterpolationAtHalfRate();
    testLinearInterpolationAcrossNegativeSlope();
    testContinuousLoopWrapsDeterministically();
    testUntilReleaseLeavesLoopAndPlaysTail();
    testReleaseEnvelopeCompletesOnContinuousLoop();
    testDuplicateSameNoteStacksAndNoteOffReleasesAllMatches();
    testExclusiveClassPreservesLayersFromSameNoteInstance();
    testExclusiveClassIsScopedToMidiChannel();
    testExclusiveReplacementAtAdmissionLimitDoesNotRequireStealing();
    testExclusiveRejectedIncomingNeverSacrificesExistingVoice();
    testExclusiveTailBudgetRejectsWithoutPartialChoke();
    testExclusiveReplacementDoesNotFightLoweredAdmissionTarget();
    testHardVoiceCapAndRejectedStartAreBounded();
    testInvalidLoopAndZeroPhaseRejectWithoutLeakingRef();
    std::cout << "voice renderer tests passed\n";
    return 0;
}
