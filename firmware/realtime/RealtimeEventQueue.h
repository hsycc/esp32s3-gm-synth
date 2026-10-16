#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>

#if defined(ESP_PLATFORM)
#include "xtensa/config/core-isa.h"
#endif

namespace gm_synth::realtime {

enum class EventType : std::uint8_t {
    NoteOn = 0,
    NoteOff,
    ControlChange,
    ProgramChange,
    PitchBend,
    // A MIDI decoder may translate the GM System On SysEx message into this
    // compact system-level event before it enters the realtime queue. The audio
    // side performs only bounded state reset + Program Change handoff work.
    GmReset,
};

struct RealtimeEvent {
    std::uint64_t timestamp_frame = 0;
    EventType type = EventType::NoteOn;
    std::uint8_t channel = 0;
    std::uint8_t data1 = 0;
    std::uint8_t data2 = 0;
    // PitchBend uses the full 14-bit value here (0..16383, center 8192).
    std::uint16_t value = 0;

    bool valid() const { return channel < 16; }
};

static_assert(sizeof(RealtimeEvent) <= 16, "RealtimeEvent must remain compact");

// libstdc++'s std::atomic<T>::is_always_lock_free is conservative on the
// ESP-IDF Xtensa toolchain even though ESP-IDF provides 32-bit atomic builtins
// and ESP32-S3 has the required hardware instructions. Gate the actual target
// capability instead of rejecting a supported target because of that trait.
// RealtimeEventQueue instances must remain in internal RAM, not PSRAM.
#if defined(ESP_PLATFORM)
static_assert(XCHAL_HAVE_S32C1I == 1, "M5 requires ESP32-S3 32-bit atomic CAS support");
static_assert(XCHAL_HAVE_RELEASE_SYNC == 1, "M5 requires Xtensa acquire/release sync support");
#else
static_assert(
    std::atomic<std::uint32_t>::is_always_lock_free,
    "M5 host tests require lock-free 32-bit atomics");
#endif

struct RealtimeEventQueueStats {
    std::uint32_t submitted = 0;
    std::uint32_t accepted = 0;
    std::uint32_t rejected_invalid = 0;
    std::uint32_t rejected_full = 0;
    std::uint32_t rejected_out_of_order = 0;
    std::uint32_t popped = 0;
};

// Single-producer/single-consumer bounded ring. Producer timestamps must be
// nondecreasing so the audio consumer never sorts or allocates.
//
// Placement contract: keep this object in internal RAM. ESP32-S3 S32C1I is a
// correct realtime primitive for internal memory; PSRAM atomic access has a
// different ESP-IDF workaround path and is intentionally outside this queue's
// hot-path contract.
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

    // 32-bit atomics are naturally aligned on ESP32-S3. Avoid desktop-style
    // cache-line over-alignment here: Xtensa does not need 64-byte alignment
    // for SPSC correctness, and excessive alignment can exceed target limits.
    std::atomic<std::uint32_t> head_{0};
    std::atomic<std::uint32_t> tail_{0};

    bool producer_has_timestamp_ = false;
    std::uint64_t producer_last_timestamp_ = 0;

    std::atomic<std::uint32_t> submitted_{0};
    std::atomic<std::uint32_t> accepted_{0};
    std::atomic<std::uint32_t> rejected_invalid_{0};
    std::atomic<std::uint32_t> rejected_full_{0};
    std::atomic<std::uint32_t> rejected_out_of_order_{0};
    std::atomic<std::uint32_t> popped_{0};
};

}  // namespace gm_synth::realtime
