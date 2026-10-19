#include "realtime/RealtimeHealthTracker.h"

#include <cassert>
#include <cstdint>

namespace {

using gm_synth::realtime::RealtimeHealthTracker;

void testTransientBurstRecoversInsideDmaWindow() {
    RealtimeHealthTracker tracker(1451u, 11610u);
    tracker.addBlock(1920u, 1920u);
    tracker.addBlock(800u, 800u);
    tracker.addBlock(900u, 900u);

    const auto stats = tracker.snapshot();
    assert(stats.render_soft_overruns == 1u);
    assert(stats.producer_work_soft_overruns == 1u);
    assert(stats.max_consecutive_work_overruns == 1u);
    assert(stats.max_backlog_us == 469u);
    assert(stats.final_backlog_us == 0u);
    assert(stats.max_backlog_blocks == 1u);
    assert(stats.max_backlog_recovery_blocks == 1u);
    assert(stats.min_dma_margin_us == 11141u);
    assert(!stats.backlog_exceeded_dma_window);
    assert(stats.backlogHealthy());
}

void testRendererAndProducerWorkOverrunsRemainSeparate() {
    RealtimeHealthTracker tracker(1451u, 11610u);
    tracker.addBlock(1400u, 1500u);
    tracker.addBlock(1400u, 1300u);

    const auto stats = tracker.snapshot();
    assert(stats.render_soft_overruns == 0u);
    assert(stats.producer_work_soft_overruns == 1u);
    assert(stats.max_backlog_us == 49u);
    assert(stats.final_backlog_us == 0u);
    assert(stats.backlogHealthy());
}

void testConsecutiveOverrunsAreCharacterized() {
    RealtimeHealthTracker tracker(1451u, 11610u);
    tracker.addBlock(1600u, 1600u);
    tracker.addBlock(1650u, 1650u);
    tracker.addBlock(1000u, 1000u);

    const auto stats = tracker.snapshot();
    assert(stats.render_soft_overruns == 2u);
    assert(stats.producer_work_soft_overruns == 2u);
    assert(stats.max_consecutive_work_overruns == 2u);
    assert(stats.max_backlog_us == 348u);
    assert(stats.final_backlog_us == 0u);
    assert(stats.max_backlog_recovery_blocks == 1u);
    assert(stats.backlogHealthy());
}

void testUnrecoveredBacklogIsHardFailure() {
    RealtimeHealthTracker tracker(1451u, 11610u);
    tracker.addBlock(1900u, 1900u);

    const auto stats = tracker.snapshot();
    assert(stats.max_backlog_us == 449u);
    assert(stats.final_backlog_us == 449u);
    assert(!stats.backlog_exceeded_dma_window);
    assert(!stats.backlogHealthy());
}

void testBacklogCrossingDmaWindowIsHardFailure() {
    RealtimeHealthTracker tracker(1451u, 11610u);
    for (std::uint32_t block = 0; block < 48u; ++block) {
        tracker.addBlock(1700u, 1700u);
    }

    const auto stats = tracker.snapshot();
    assert(stats.max_consecutive_work_overruns == 48u);
    assert(stats.max_backlog_us == 11952u);
    assert(stats.max_backlog_blocks == 9u);
    assert(stats.min_dma_margin_us == 0u);
    assert(stats.backlog_exceeded_dma_window);
    assert(!stats.backlogHealthy());
}

}  // namespace

int main() {
    testTransientBurstRecoversInsideDmaWindow();
    testRendererAndProducerWorkOverrunsRemainSeparate();
    testConsecutiveOverrunsAreCharacterized();
    testUnrecoveredBacklogIsHardFailure();
    testBacklogCrossingDmaWindowIsHardFailure();
    return 0;
}
