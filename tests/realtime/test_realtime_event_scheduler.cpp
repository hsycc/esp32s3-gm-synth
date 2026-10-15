#include "../../firmware/realtime/BlockEventScheduler.h"

#include <cassert>
#include <cstdint>
#include <iostream>

namespace {

using gm_synth::realtime::BlockEventScheduler;
using gm_synth::realtime::EventType;
using gm_synth::realtime::RealtimeEvent;
using gm_synth::realtime::RealtimeEventQueue;

RealtimeEvent note(
    std::uint64_t frame,
    std::uint8_t key,
    EventType type = EventType::NoteOn) {
    RealtimeEvent event{};
    event.timestamp_frame = frame;
    event.type = type;
    event.channel = 0;
    event.data1 = key;
    event.data2 = 100;
    return event;
}

void testOffsetsZeroOneAndLastFrame() {
    RealtimeEventQueue queue;
    assert(queue.tryPush(note(1000, 60)));
    assert(queue.tryPush(note(1001, 61)));
    assert(queue.tryPush(note(1063, 62)));

    BlockEventScheduler scheduler(queue);
    const auto& batch = scheduler.prepareBlock(1000, 64);
    assert(batch.count == 3);
    assert(batch.events[0].frame_offset == 0);
    assert(batch.events[1].frame_offset == 1);
    assert(batch.events[2].frame_offset == 63);
    assert(batch.events[0].event.data1 == 60);
    assert(batch.events[1].event.data1 == 61);
    assert(batch.events[2].event.data1 == 62);
    assert(queue.empty());
}

void testEndBoundaryRemainsForNextBlock() {
    RealtimeEventQueue queue;
    assert(queue.tryPush(note(1064, 64)));

    BlockEventScheduler scheduler(queue);
    const auto& first = scheduler.prepareBlock(1000, 64);
    assert(first.count == 0);
    assert(queue.size() == 1);

    const auto& second = scheduler.prepareBlock(1064, 64);
    assert(second.count == 1);
    assert(second.events[0].frame_offset == 0);
    assert(second.events[0].event.data1 == 64);
    assert(queue.empty());
}

void testFutureEventIsRetained() {
    RealtimeEventQueue queue;
    assert(queue.tryPush(note(2000, 65)));

    BlockEventScheduler scheduler(queue);
    const auto& batch = scheduler.prepareBlock(1000, 64);
    assert(batch.count == 0);
    assert(queue.size() == 1);

    RealtimeEvent peeked{};
    assert(queue.tryPeek(&peeked));
    assert(peeked.timestamp_frame == 2000);
}

void testLateEventClampsAndCounts() {
    RealtimeEventQueue queue;
    assert(queue.tryPush(note(900, 66)));
    assert(queue.tryPush(note(1000, 67)));

    BlockEventScheduler scheduler(queue);
    const auto& batch = scheduler.prepareBlock(1000, 64);
    assert(batch.count == 2);
    assert(batch.events[0].frame_offset == 0);
    assert(batch.events[1].frame_offset == 0);
    assert(scheduler.stats().late_events == 1);
}

void testEqualTimestampPreservesFifo() {
    RealtimeEventQueue queue;
    assert(queue.tryPush(note(1005, 60)));
    assert(queue.tryPush(note(1005, 61)));
    assert(queue.tryPush(note(1005, 62, EventType::NoteOff)));

    BlockEventScheduler scheduler(queue);
    const auto& batch = scheduler.prepareBlock(1000, 64);
    assert(batch.count == 3);
    assert(batch.events[0].event.data1 == 60);
    assert(batch.events[1].event.data1 == 61);
    assert(batch.events[2].event.data1 == 62);
    assert(batch.events[0].frame_offset == 5);
    assert(batch.events[1].frame_offset == 5);
    assert(batch.events[2].frame_offset == 5);
}

void testOutOfOrderTimestampIsRejected() {
    RealtimeEventQueue queue;
    assert(queue.tryPush(note(100, 60)));
    assert(!queue.tryPush(note(99, 61)));
    assert(queue.size() == 1);
    const auto stats = queue.stats();
    assert(stats.submitted == 2);
    assert(stats.accepted == 1);
    assert(stats.rejected_out_of_order == 1);
}

void testInvalidChannelIsRejected() {
    RealtimeEventQueue queue;
    RealtimeEvent event = note(100, 60);
    event.channel = 16;
    assert(!queue.tryPush(event));
    const auto stats = queue.stats();
    assert(stats.rejected_invalid == 1);
    assert(queue.empty());
}

void testFullQueueIsExplicitAndRecoverable() {
    RealtimeEventQueue queue;
    for (std::size_t i = 0; i < RealtimeEventQueue::kCapacity; ++i) {
        assert(queue.tryPush(note(i, static_cast<std::uint8_t>(i & 0x7f))));
    }
    assert(queue.size() == RealtimeEventQueue::kCapacity);
    assert(!queue.tryPush(note(RealtimeEventQueue::kCapacity, 1)));
    assert(queue.stats().rejected_full == 1);

    RealtimeEvent popped{};
    assert(queue.tryPop(&popped));
    assert(popped.timestamp_frame == 0);
    assert(queue.tryPush(note(RealtimeEventQueue::kCapacity, 2)));
    assert(queue.size() == RealtimeEventQueue::kCapacity);
}

void testFullBatchDrainWithEqualTimestamp() {
    RealtimeEventQueue queue;
    for (std::size_t i = 0; i < RealtimeEventQueue::kCapacity; ++i) {
        assert(queue.tryPush(note(1001, static_cast<std::uint8_t>(i & 0x7f))));
    }

    BlockEventScheduler scheduler(queue);
    const auto& batch = scheduler.prepareBlock(1000, 64);
    assert(batch.count == RealtimeEventQueue::kCapacity);
    assert(queue.empty());
    for (std::size_t i = 0; i < batch.count; ++i) {
        assert(batch.events[i].frame_offset == 1);
        assert(batch.events[i].event.data1 == static_cast<std::uint8_t>(i & 0x7f));
    }
    assert(scheduler.stats().events_dispatched == RealtimeEventQueue::kCapacity);
}

void testZeroLengthBlockDoesNotConsume() {
    RealtimeEventQueue queue;
    assert(queue.tryPush(note(1000, 70)));
    BlockEventScheduler scheduler(queue);
    const auto& batch = scheduler.prepareBlock(1000, 0);
    assert(batch.count == 0);
    assert(queue.size() == 1);
    assert(scheduler.stats().empty_blocks == 1);
}

}  // namespace

int main() {
    testOffsetsZeroOneAndLastFrame();
    testEndBoundaryRemainsForNextBlock();
    testFutureEventIsRetained();
    testLateEventClampsAndCounts();
    testEqualTimestampPreservesFifo();
    testOutOfOrderTimestampIsRejected();
    testInvalidChannelIsRejected();
    testFullQueueIsExplicitAndRecoverable();
    testFullBatchDrainWithEqualTimestamp();
    testZeroLengthBlockDoesNotConsume();
    std::cout << "realtime event scheduler tests passed\n";
    return 0;
}
