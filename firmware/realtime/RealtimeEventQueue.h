#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>

namespace gm_synth::realtime {

enum class EventType : std::uint8_t {
    NoteOn = 0,
    NoteOff,
    ControlChange,
    ProgramChange,
};

// Compact timestamped event crossing from the MIDI/control producer into the
// audio consumer. timestamp_frame is an absolute frame counter at the 44.1-kHz
// render timeline; equal timestamps preserve producer FIFO order.
struct RealtimeEvent {
    std::uint64_t timestamp_frame = 0;
    EventType type = EventType::NoteOn;
    std::uint8_t channel = 0;
    std::uint8_t data1 = 0;
    std::uint8_t data2 = 0;
    std::uint16_t value = 0;

    bool valid() const { return channel < 16; }
};

static_assert(sizeof(RealtimeEvent) <= 16, "RealtimeEvent must remain compact");

struct RealtimeEventQueueStats {
    std::uint64_t submitted = 0;
    std::uint64_t accepted = 0;
    std::uint64_t rejected_invalid = 0;
    std::uint64_t rejected_full = 0;
    std::uint64_t rejected_out_of_order = 0;
    std::uint64_t popped = 0;
};

// Single-producer/single-consumer bounded ring.
//
// Producer: MIDI/control side, one owner only.
// Consumer: audio side, one owner only.
//
// No allocation, no mutex, no blocking. Producer timestamps must be
// nondecreasing; this makes block draining O(events-due) with no audio-side
// sorting and gives equal-timestamp FIFO ordering for free.
class RealtimeEventQueue {
public:
    static constexpr std::size_t kCapacity = 128;

    bool tryPush(const RealtimeEvent& event);
    bool tryPeek(RealtimeEvent* event) const;
    bool tryPop(RealtimeEvent* event);

    std::size_t size() const;
    bool empty() const { return size() == 0; }
    RealtimeEventQueueStats stats() const;

private:
    std::array<RealtimeEvent, kCapacity> events_{};

    // Monotonic counters avoid ambiguous full/empty states. Unsigned wrap is
    // well-defined; capacity is tiny relative to the 32-bit counter range.
    alignas(64) std::atomic<std::uint32_t> head_{0};
    alignas(64) std::atomic<std::uint32_t> tail_{0};

    // Producer-owned ordering state.
    bool producer_has_timestamp_ = false;
    std::uint64_t producer_last_timestamp_ = 0;

    std::atomic<std::uint64_t> submitted_{0};
    std::atomic<std::uint64_t> accepted_{0};
    std::atomic<std::uint64_t> rejected_invalid_{0};
    std::atomic<std::uint64_t> rejected_full_{0};
    std::atomic<std::uint64_t> rejected_out_of_order_{0};
    std::atomic<std::uint64_t> popped_{0};
};

}  // namespace gm_synth::realtime
