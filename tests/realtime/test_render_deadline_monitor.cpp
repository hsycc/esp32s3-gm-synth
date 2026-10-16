#include "../../firmware/realtime/RenderDeadlineMonitor.h"
#include "../../firmware/sample/PublishedSampleDirectory.h"

#include <cassert>
#include <cstdint>
#include <iostream>

namespace {

using gm_synth::realtime::RenderDeadlineMonitor;
using gm_synth::sample::PublishedSampleDirectory;
using gm_synth::voice::AdaptivePolyphonyConfig;
using gm_synth::voice::AdaptivePolyphonyController;
using gm_synth::voice::VoicePool;

AdaptivePolyphonyConfig quietConfig() {
    AdaptivePolyphonyConfig config{};
    config.block_budget_us = 10000;
    config.high_pressure_percent = 90;
    config.recovery_pressure_percent = 10;
    config.high_pressure_blocks = 8;
    config.recovery_blocks = 512;
    config.min_limit = 2;
    config.initial_limit = 8;
    config.max_limit = 8;
    return config;
}

void testEmptySnapshot() {
    PublishedSampleDirectory directory;
    VoicePool voices(directory);
    AdaptivePolyphonyController controller(voices, quietConfig());
    RenderDeadlineMonitor monitor(controller);

    const auto snapshot = monitor.snapshot();
    assert(snapshot.blocks_observed == 0);
    assert(snapshot.sample_count == 0);
    assert(snapshot.min_us == 0);
    assert(snapshot.p99_us == 0);
    assert(snapshot.max_us == 0);
    assert(snapshot.admission_limit == 8);
}

void testFixedWindowPercentiles() {
    PublishedSampleDirectory directory;
    VoicePool voices(directory);
    AdaptivePolyphonyController controller(voices, quietConfig());
    RenderDeadlineMonitor monitor(controller);

    assert(monitor.observeBlock(400) == 8);
    assert(monitor.observeBlock(100) == 8);
    assert(monitor.observeBlock(300) == 8);
    assert(monitor.observeBlock(200) == 8);

    const auto snapshot = monitor.snapshot();
    assert(snapshot.blocks_observed == 4);
    assert(snapshot.deadline_misses == 0);
    assert(snapshot.sample_count == 4);
    assert(snapshot.min_us == 100);
    assert(snapshot.avg_us == 250);
    assert(snapshot.p50_us == 200);
    assert(snapshot.p95_us == 400);
    assert(snapshot.p99_us == 400);
    assert(snapshot.max_us == 400);
    assert(monitor.stats().last_render_us == 200);
    assert(monitor.stats().peak_render_us == 400);
}

void testWindowWrapKeepsNewestSamples() {
    PublishedSampleDirectory directory;
    VoicePool voices(directory);
    AdaptivePolyphonyController controller(voices, quietConfig());
    RenderDeadlineMonitor monitor(controller);

    for (std::uint32_t value = 1; value <= 130; ++value) {
        monitor.observeBlock(value);
    }

    const auto snapshot = monitor.snapshot();
    assert(snapshot.blocks_observed == 130);
    assert(snapshot.sample_count == RenderDeadlineMonitor::kWindowSize);
    assert(snapshot.min_us == 3);
    assert(snapshot.max_us == 130);
    assert(snapshot.avg_us == 66);
    assert(snapshot.p50_us == 66);
    assert(snapshot.p95_us == 124);
    assert(snapshot.p99_us == 129);
}

void testDeadlineObservationFeedsAdaptiveController() {
    PublishedSampleDirectory directory;
    VoicePool voices(directory);

    AdaptivePolyphonyConfig config{};
    config.block_budget_us = 1000;
    config.high_pressure_percent = 80;
    config.recovery_pressure_percent = 50;
    config.high_pressure_blocks = 4;
    config.recovery_blocks = 128;
    config.min_limit = 2;
    config.initial_limit = 8;
    config.max_limit = 8;

    AdaptivePolyphonyController controller(voices, config);
    RenderDeadlineMonitor monitor(controller);

    assert(monitor.observeBlock(100, true) == 6);
    assert(monitor.stats().deadline_misses == 1);
    assert(controller.stats().deadline_misses == 1);
    assert(voices.admissionLimit() == 6);

    // Crossing the configured budget is also a miss even if the caller did
    // not separately report an I2S/deadline failure.
    assert(monitor.observeBlock(1000, false) == 5);
    assert(monitor.stats().deadline_misses == 2);
    assert(controller.stats().deadline_misses == 2);
    assert(voices.admissionLimit() == 5);
}

}  // namespace

int main() {
    testEmptySnapshot();
    testFixedWindowPercentiles();
    testWindowWrapKeepsNewestSamples();
    testDeadlineObservationFeedsAdaptiveController();
    std::cout << "render deadline monitor tests passed\n";
    return 0;
}
