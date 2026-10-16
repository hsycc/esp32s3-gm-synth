#include "../../firmware/voice/AdaptivePolyphony.h"
#include "../../firmware/sample/PublishedSampleDirectory.h"

#include <array>
#include <cassert>
#include <cstdint>
#include <iostream>

namespace {

using gm_synth::sample::PublishedSampleDirectory;
using gm_synth::sample::ResidentRecord;
using gm_synth::sample::SampleDescriptor;
using gm_synth::voice::AdaptivePolyphonyConfig;
using gm_synth::voice::AdaptivePolyphonyController;
using gm_synth::voice::LoopMode;
using gm_synth::voice::VoicePool;
using gm_synth::voice::VoiceStartParams;
using gm_synth::voice::VoiceStealConfig;
using gm_synth::voice::kPhaseOneQ32;
using gm_synth::voice::kQ15Unity;

struct PublishedFixture {
    PublishedSampleDirectory directory;
    ResidentRecord record{};
    std::array<std::int16_t, 64> pcm{};

    PublishedFixture() {
        pcm.fill(1000);
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

VoiceStartParams paramsFor(std::uint8_t key) {
    VoiceStartParams params{};
    params.sample_id = 0;
    params.channel = 0;
    params.key = key;
    params.velocity = 100;
    params.phase_increment_q32 = kPhaseOneQ32;
    params.loop_mode = LoopMode::Continuous;
    params.gain_left_q15 = kQ15Unity;
    params.gain_right_q15 = kQ15Unity;
    params.envelope.sustain_q15 = kQ15Unity;
    params.envelope.release_frames = 0;
    return params;
}

AdaptivePolyphonyConfig testConfig() {
    AdaptivePolyphonyConfig config{};
    config.block_budget_us = 1000;
    config.high_pressure_percent = 80;
    config.recovery_pressure_percent = 50;
    config.high_pressure_blocks = 2;
    config.recovery_blocks = 3;
    config.min_limit = 2;
    config.initial_limit = 8;
    config.max_limit = 8;
    return config;
}

VoiceStealConfig stealConfig(std::uint32_t fade_frames, std::size_t max_tails) {
    VoiceStealConfig config{};
    config.enabled = true;
    config.fade_frames = fade_frames;
    config.max_tail_voices = max_tails;
    return config;
}

void testAdmissionLimitNeverCutsExistingVoices() {
    PublishedFixture fixture;
    VoicePool voices(fixture.directory);
    voices.setAdmissionLimit(4);

    for (std::uint8_t key = 60; key < 64; ++key) {
        assert(voices.start(paramsFor(key)));
    }
    assert(voices.activeVoiceCount() == 4);

    voices.setAdmissionLimit(2);
    assert(voices.activeVoiceCount() == 4);
    assert(!voices.start(paramsFor(70)));
    assert(voices.stats().start_failures_admission == 1);

    voices.allSoundOff();
    assert(voices.activeVoiceCount() == 0);
    assert(voices.start(paramsFor(60)));
    assert(voices.start(paramsFor(61)));
    assert(!voices.start(paramsFor(62)));
    assert(voices.activeVoiceCount() == 2);
    voices.allSoundOff();
}

void testDeadlineMissReducesAdmissionQuickly() {
    PublishedFixture fixture;
    VoicePool voices(fixture.directory);
    AdaptivePolyphonyController controller(voices, testConfig());

    assert(voices.admissionLimit() == 8);
    assert(controller.observeBlock(1000, false) == 6);
    assert(controller.stats().deadline_misses == 1);
    assert(controller.stats().limit_reductions == 1);

    assert(controller.observeBlock(1100, true) == 5);
    assert(controller.stats().deadline_misses == 2);
    assert(controller.stats().minimum_limit_observed == 5);
}

void testSustainedHighPressureUsesHysteresis() {
    PublishedFixture fixture;
    VoicePool voices(fixture.directory);
    auto config = testConfig();
    config.initial_limit = 6;
    AdaptivePolyphonyController controller(voices, config);

    assert(controller.observeBlock(850, false) == 6);
    assert(controller.observeBlock(850, false) == 5);
    assert(controller.stats().high_pressure_events == 1);
    assert(controller.stats().limit_reductions == 1);

    // Mid-band pressure breaks the streak instead of oscillating around the
    // high-pressure threshold.
    assert(controller.observeBlock(700, false) == 5);
    assert(controller.observeBlock(850, false) == 5);
    assert(controller.observeBlock(700, false) == 5);
}

void testStableLowPressureRecoversGradually() {
    PublishedFixture fixture;
    VoicePool voices(fixture.directory);
    auto config = testConfig();
    config.initial_limit = 4;
    config.max_limit = 6;
    AdaptivePolyphonyController controller(voices, config);

    assert(controller.observeBlock(400, false) == 4);
    assert(controller.observeBlock(400, false) == 4);
    assert(controller.observeBlock(400, false) == 5);
    assert(controller.stats().limit_recoveries == 1);

    // One mid-band block resets recovery hysteresis.
    assert(controller.observeBlock(700, false) == 5);
    assert(controller.observeBlock(400, false) == 5);
    assert(controller.observeBlock(400, false) == 5);
    assert(controller.observeBlock(400, false) == 6);
    assert(controller.stats().limit_recoveries == 2);
    assert(controller.stats().maximum_limit_observed == 6);
}

void testConfigurationIsBoundedByHardCap() {
    PublishedFixture fixture;
    VoicePool voices(fixture.directory);
    AdaptivePolyphonyConfig config{};
    config.block_budget_us = 0;
    config.high_pressure_percent = 0;
    config.recovery_pressure_percent = 100;
    config.high_pressure_blocks = 0;
    config.recovery_blocks = 0;
    config.min_limit = 0;
    config.initial_limit = 999;
    config.max_limit = 999;

    AdaptivePolyphonyController controller(voices, config);
    assert(controller.config().block_budget_us == 1);
    assert(controller.config().min_limit == 1);
    assert(controller.config().max_limit == VoicePool::kHardVoiceCap);
    assert(controller.stats().current_limit == VoicePool::kHardVoiceCap);
}

void testStealingDoesNotFightARecentlyLoweredAdmissionTarget() {
    PublishedFixture fixture;
    VoicePool voices(fixture.directory);
    voices.setAdmissionLimit(4);
    voices.setVoiceStealConfig(stealConfig(4, 2));

    for (std::uint8_t key = 60; key < 64; ++key) {
        assert(voices.start(paramsFor(key)));
    }
    voices.setAdmissionLimit(2);

    assert(!voices.start(paramsFor(70)));
    assert(voices.activeVoiceCount() == 4);
    assert(voices.activeStealTailCount() == 0);
    assert(voices.stats().steal_failures_over_target == 1);
    assert(voices.stats().steal_successes == 0);
    voices.allSoundOff();
}

void testStealPrefersReleasingVoiceAndKeepsRefUntilFadeCompletes() {
    PublishedFixture fixture;
    VoicePool voices(fixture.directory);
    voices.setAdmissionLimit(2);
    voices.setVoiceStealConfig(stealConfig(4, 1));

    auto first = paramsFor(60);
    first.envelope.release_frames = 64;
    assert(voices.start(first));
    assert(voices.start(paramsFor(61)));
    assert(fixture.directory.stats().live_voice_refs == 2);

    assert(voices.noteOff(0, 60) == 1);
    assert(voices.start(paramsFor(62)));
    assert(voices.activeVoiceCount() == 3);
    assert(voices.activeStealTailCount() == 1);
    assert(fixture.directory.stats().live_voice_refs == 3);
    assert(voices.stats().steal_successes == 1);
    assert(voices.stats().steals_from_release == 1);
    assert(voices.stats().last_stolen_key == 60);

    std::array<std::int32_t, 8> mix{};
    voices.renderAdd(mix.data(), 4);

    // Two full-level voices remain constant at 2000; the stolen voice fades
    // over four frames instead of disappearing at the NoteOn boundary.
    assert(mix[0] > mix[2]);
    assert(mix[2] > mix[4]);
    assert(mix[4] > mix[6]);
    assert(voices.activeVoiceCount() == 2);
    assert(voices.activeStealTailCount() == 0);
    assert(fixture.directory.stats().live_voice_refs == 2);
    voices.allSoundOff();
}

void testStealTailBudgetBoundsTransientOversubscription() {
    PublishedFixture fixture;
    VoicePool voices(fixture.directory);
    voices.setAdmissionLimit(2);
    voices.setVoiceStealConfig(stealConfig(8, 1));

    assert(voices.start(paramsFor(60)));
    assert(voices.start(paramsFor(61)));
    assert(voices.start(paramsFor(62)));
    assert(voices.activeVoiceCount() == 3);
    assert(voices.activeStealTailCount() == 1);
    assert(voices.stats().last_stolen_key == 60);

    assert(!voices.start(paramsFor(63)));
    assert(voices.stats().steal_failures_tail_budget == 1);
    assert(voices.activeVoiceCount() == 3);

    std::array<std::int32_t, 16> mix{};
    voices.renderAdd(mix.data(), 8);
    assert(voices.activeVoiceCount() == 2);
    assert(voices.activeStealTailCount() == 0);

    assert(voices.start(paramsFor(63)));
    assert(voices.activeVoiceCount() == 3);
    assert(voices.activeStealTailCount() == 1);
    assert(voices.stats().last_stolen_key == 61);
    voices.allSoundOff();
}

void testRejectedIncomingSampleNeverSacrificesVictim() {
    PublishedFixture fixture;
    VoicePool voices(fixture.directory);
    voices.setAdmissionLimit(2);
    voices.setVoiceStealConfig(stealConfig(4, 1));

    assert(voices.start(paramsFor(60)));
    assert(voices.start(paramsFor(61)));
    const auto refs_before = fixture.directory.stats().live_voice_refs;

    auto missing = paramsFor(62);
    missing.sample_id = 1;
    assert(!voices.start(missing));
    assert(voices.activeVoiceCount() == 2);
    assert(voices.activeStealTailCount() == 0);
    assert(fixture.directory.stats().live_voice_refs == refs_before);
    assert(voices.stats().steal_successes == 0);
    assert(voices.stats().start_failures_rejected == 1);
    voices.allSoundOff();
}

}  // namespace

int main() {
    testAdmissionLimitNeverCutsExistingVoices();
    testDeadlineMissReducesAdmissionQuickly();
    testSustainedHighPressureUsesHysteresis();
    testStableLowPressureRecoversGradually();
    testConfigurationIsBoundedByHardCap();
    testStealingDoesNotFightARecentlyLoweredAdmissionTarget();
    testStealPrefersReleasingVoiceAndKeepsRefUntilFadeCompletes();
    testStealTailBudgetBoundsTransientOversubscription();
    testRejectedIncomingSampleNeverSacrificesVictim();
    std::cout << "adaptive polyphony tests passed\n";
    return 0;
}
