#include "../../firmware/realtime/BlockEventScheduler.h"
#include "../../firmware/realtime/RealtimeEventQueue.h"
#include "../../firmware/realtime/RenderDeadlineMonitor.h"
#include "../../firmware/realtime/ScheduledAudioBlockRenderer.h"
#include "../../firmware/realtime/TimedAudioBlockRenderer.h"
#include "../../firmware/sample/PublishedSampleDirectory.h"

#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <limits>

namespace {

using gm_synth::realtime::BlockEventScheduler;
using gm_synth::realtime::RealtimeEventQueue;
using gm_synth::realtime::RenderDeadlineMonitor;
using gm_synth::realtime::ScheduledAudioBlockRenderer;
using gm_synth::realtime::TimedAudioBlockRenderer;
using gm_synth::sample::PublishedSampleDirectory;
using gm_synth::voice::AdaptivePolyphonyConfig;
using gm_synth::voice::AdaptivePolyphonyController;
using gm_synth::voice::VoicePool;

struct FakeClock {
    std::array<std::uint64_t, 4> values{};
    std::size_t count = 0;
    std::size_t index = 0;
};

std::uint64_t fakeNowUs(void* context) {
    auto* clock = static_cast<FakeClock*>(context);
    assert(clock != nullptr);
    assert(clock->index < clock->count);
    return clock->values[clock->index++];
}

AdaptivePolyphonyConfig configFor(std::uint32_t budget_us = 1000) {
    AdaptivePolyphonyConfig config{};
    config.block_budget_us = budget_us;
    config.high_pressure_percent = 80;
    config.recovery_pressure_percent = 50;
    config.high_pressure_blocks = 4;
    config.recovery_blocks = 128;
    config.min_limit = 2;
    config.initial_limit = 8;
    config.max_limit = 8;
    return config;
}

struct RenderFixture {
    PublishedSampleDirectory directory;
    VoicePool voices{directory};
    RealtimeEventQueue queue;
    BlockEventScheduler scheduler{queue};
    ScheduledAudioBlockRenderer renderer{voices, scheduler, nullptr};
    AdaptivePolyphonyController controller;
    RenderDeadlineMonitor monitor;
    std::array<std::int16_t, ScheduledAudioBlockRenderer::kSamplesPerBlock> output{};

    explicit RenderFixture(std::uint32_t budget_us = 1000)
        : controller(voices, configFor(budget_us)),
          monitor(controller) {}
};

void testMeasuredRenderFeedsMonitor() {
    RenderFixture fixture;
    FakeClock clock{};
    clock.values[0] = 1000;
    clock.values[1] = 1234;
    clock.count = 2;

    TimedAudioBlockRenderer timed(
        fixture.renderer,
        fixture.monitor,
        fakeNowUs,
        &clock);

    assert(timed.render(0, fixture.output.data()) == 8);
    assert(timed.stats().blocks_timed == 1);
    assert(timed.stats().clock_faults == 0);
    assert(timed.stats().last_render_us == 234);
    assert(fixture.renderer.stats().blocks_rendered == 1);

    const auto snapshot = fixture.monitor.snapshot();
    assert(snapshot.blocks_observed == 1);
    assert(snapshot.sample_count == 1);
    assert(snapshot.min_us == 234);
    assert(snapshot.avg_us == 234);
    assert(snapshot.max_us == 234);
}

void testMissingClockDoesNotPoisonAdaptivePolicy() {
    RenderFixture fixture;
    TimedAudioBlockRenderer timed(
        fixture.renderer,
        fixture.monitor,
        nullptr,
        nullptr);

    assert(timed.render(0, fixture.output.data()) == 8);
    assert(fixture.renderer.stats().blocks_rendered == 1);
    assert(timed.stats().blocks_timed == 0);
    assert(timed.stats().clock_faults == 1);
    assert(fixture.monitor.stats().blocks_observed == 0);
    assert(fixture.controller.stats().blocks_observed == 0);
    assert(fixture.voices.admissionLimit() == 8);
}

void testClockRegressionIsRejected() {
    RenderFixture fixture;
    FakeClock clock{};
    clock.values[0] = 2000;
    clock.values[1] = 1999;
    clock.count = 2;

    TimedAudioBlockRenderer timed(
        fixture.renderer,
        fixture.monitor,
        fakeNowUs,
        &clock);

    assert(timed.render(0, fixture.output.data()) == 8);
    assert(timed.stats().blocks_timed == 0);
    assert(timed.stats().clock_faults == 1);
    assert(fixture.monitor.stats().blocks_observed == 0);
    assert(fixture.voices.admissionLimit() == 8);
}

void testExternalDeadlineMissIsForwarded() {
    RenderFixture fixture;
    FakeClock clock{};
    clock.values[0] = 5000;
    clock.values[1] = 5100;
    clock.count = 2;

    TimedAudioBlockRenderer timed(
        fixture.renderer,
        fixture.monitor,
        fakeNowUs,
        &clock);

    assert(timed.render(0, fixture.output.data(), true) == 6);
    assert(timed.stats().last_render_us == 100);
    assert(fixture.monitor.stats().deadline_misses == 1);
    assert(fixture.controller.stats().deadline_misses == 1);
    assert(fixture.voices.admissionLimit() == 6);
}

void testUnrepresentableDurationClampsAndTripsDeadline() {
    RenderFixture fixture;
    FakeClock clock{};
    clock.values[0] = 0;
    clock.values[1] =
        static_cast<std::uint64_t>(std::numeric_limits<std::uint32_t>::max()) + 10u;
    clock.count = 2;

    TimedAudioBlockRenderer timed(
        fixture.renderer,
        fixture.monitor,
        fakeNowUs,
        &clock);

    assert(timed.render(0, fixture.output.data()) == 6);
    assert(timed.stats().blocks_timed == 1);
    assert(timed.stats().duration_clamps == 1);
    assert(timed.stats().last_render_us == std::numeric_limits<std::uint32_t>::max());
    assert(fixture.monitor.stats().deadline_misses == 1);
}

}  // namespace

int main() {
    testMeasuredRenderFeedsMonitor();
    testMissingClockDoesNotPoisonAdaptivePolicy();
    testClockRegressionIsRejected();
    testExternalDeadlineMissIsForwarded();
    testUnrepresentableDurationClampsAndTripsDeadline();
    std::cout << "timed audio block renderer tests passed\n";
    return 0;
}
