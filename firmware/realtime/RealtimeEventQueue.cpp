#include "RealtimeEventQueue.h"

#include <algorithm>

namespace gm_synth::realtime {

bool RealtimeEventQueue::tryPush(const RealtimeEvent& event) {
    submitted_.fetch_add(1, std::memory_order_relaxed);

    if (!event.valid()) {
        rejected_invalid_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    if (producer_has_timestamp_ && event.timestamp_frame < producer_last_timestamp_) {
        rejected_out_of_order_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }

    const std::uint32_t head = head_.load(std::memory_order_relaxed);
    const std::uint32_t tail = tail_.load(std::memory_order_acquire);
    if (static_cast<std::uint32_t>(head - tail) >= kCapacity) {
        rejected_full_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }

    events_[head % kCapacity] = event;
    head_.store(head + 1u, std::memory_order_release);
    producer_has_timestamp_ = true;
    producer_last_timestamp_ = event.timestamp_frame;
    accepted_.fetch_add(1, std::memory_order_relaxed);
    return true;
}

bool RealtimeEventQueue::tryPeek(RealtimeEvent* event) const {
    if (event == nullptr) {
        return false;
    }
    const std::uint32_t tail = tail_.load(std::memory_order_relaxed);
    const std::uint32_t head = head_.load(std::memory_order_acquire);
    if (tail == head) {
        return false;
    }
    *event = events_[tail % kCapacity];
    return true;
}

bool RealtimeEventQueue::tryPop(RealtimeEvent* event) {
    if (event == nullptr) {
        return false;
    }
    const std::uint32_t tail = tail_.load(std::memory_order_relaxed);
    const std::uint32_t head = head_.load(std::memory_order_acquire);
    if (tail == head) {
        return false;
    }
    *event = events_[tail % kCapacity];
    tail_.store(tail + 1u, std::memory_order_release);
    popped_.fetch_add(1, std::memory_order_relaxed);
    return true;
}

std::size_t RealtimeEventQueue::size() const {
    const std::uint32_t head = head_.load(std::memory_order_acquire);
    const std::uint32_t tail = tail_.load(std::memory_order_acquire);
    return std::min<std::size_t>(
        static_cast<std::uint32_t>(head - tail),
        kCapacity);
}

RealtimeEventQueueStats RealtimeEventQueue::stats() const {
    RealtimeEventQueueStats out{};
    out.submitted = submitted_.load(std::memory_order_relaxed);
    out.accepted = accepted_.load(std::memory_order_relaxed);
    out.rejected_invalid = rejected_invalid_.load(std::memory_order_relaxed);
    out.rejected_full = rejected_full_.load(std::memory_order_relaxed);
    out.rejected_out_of_order = rejected_out_of_order_.load(std::memory_order_relaxed);
    out.popped = popped_.load(std::memory_order_relaxed);
    return out;
}

}  // namespace gm_synth::realtime
