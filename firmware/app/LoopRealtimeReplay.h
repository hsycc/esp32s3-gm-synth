#pragma once

#include "realtime/RealtimeEventQueue.h"

#include <array>
#include <cstdint>

namespace gm_synth::app::loop_realtime_replay {

// An original, fixed two-part qualification phrase at 120 BPM. This is a
// reproducible event source, not a MIDI file parser or an external input port.
// Each part is queued only after its preceding Program Change has committed.
// Timestamps below are relative to that part's published start frame; source
// identity and definitionHash let Host and target prove they used this score.
inline constexpr char kName[] = "two-channel-phrase-v1";
inline constexpr std::uint32_t kSegmentFrames = 88200; // four quarter notes
inline constexpr std::uint32_t kTailFrames = 1024u * 64u;
inline constexpr std::uint16_t kChannelMask = 3;
inline constexpr std::uint64_t kHashOffset = UINT64_C(14695981039346656037);
inline constexpr std::uint64_t kHashPrime = UINT64_C(1099511628211);

constexpr realtime::RealtimeEvent event(std::uint32_t frame, realtime::EventType type,
                                       std::uint8_t channel, std::uint8_t a,
                                       std::uint8_t b = 0, std::uint16_t value = 0) {
    return {frame, type, channel, a, b, value};
}

constexpr auto segment(bool second, std::uint8_t initial, std::uint8_t replacement) {
    using T = realtime::EventType;
    // Part two uses a different melody/chord voicing while channel 1 plays the
    // replacement Program. Controller values and event times remain explicit.
    const std::uint8_t transpose = second ? 2 : 0;
    auto result = std::array{
        event(0, T::ControlChange, 0, 120, 0),
        event(0, T::ControlChange, 1, 120, 0),
        event(0, T::ControlChange, 0, 64, 0),
        event(0, T::ControlChange, 1, 64, 0),
        event(0, T::ControlChange, 0, 91, 40),
        event(0, T::ControlChange, 1, 91, 40),
        event(0, T::ControlChange, 0, 93, 20),
        event(0, T::ControlChange, 1, 93, 20),
        event(0, T::ControlChange, 0, 7, 100),
        event(0, T::ControlChange, 1, 7, 100),
        event(0, T::ControlChange, 0, 11, 127),
        event(0, T::ControlChange, 1, 11, 127),
        event(0, T::ControlChange, 0, 10, 40),
        event(0, T::ControlChange, 1, 10, 90),
        event(0, T::ControlChange, 0, 21, 0),
        event(0, T::ControlChange, 1, 21, 0),
        event(0, T::PitchBend, 0, 0, 0, 8192),
        event(0, T::PitchBend, 1, 0, 0, 8192),
        event(0, T::ChannelPressure, 0, 0),
        event(0, T::ChannelPressure, 1, 0),
        event(0, T::NoteOn, 0, 48, 64),
        event(0, T::NoteOn, 0, 55, 64),
        event(0, T::NoteOn, 0, 60, 64),
        event(0, T::NoteOn, 1, 60, 72),
        event(5519, T::NoteOff, 1, 60),
        event(11025, T::NoteOn, 1, 64, 76),
        event(16548, T::NoteOff, 1, 64),
        event(22050, T::ControlChange, 0, 64, 127),
        event(22050, T::NoteOff, 0, 48),
        event(22050, T::NoteOff, 0, 55),
        event(22050, T::NoteOff, 0, 60),
        event(22050, T::NoteOn, 1, 67, 80),
        event(27565, T::PitchBend, 1, 0, 0, 10240),
        event(33075, T::NoteOff, 1, 67),
        event(33075, T::NoteOn, 1, 69, 76),
        event(38587, T::PitchBend, 1, 0, 0, 8192),
        event(44100, T::ControlChange, 0, 64, 0),
        event(44100, T::NoteOff, 1, 69),
        event(44100, T::NoteOn, 0, 53, 64),
        event(44100, T::NoteOn, 0, 60, 64),
        event(44100, T::NoteOn, 0, 65, 64),
        event(44100, T::NoteOn, 1, 65, 72),
        event(49617, T::ChannelPressure, 1, 80),
        event(55125, T::NoteOff, 1, 65),
        event(55125, T::NoteOn, 1, 64, 76),
        event(60646, T::ControlChange, 1, 11, 96),
        event(66150, T::NoteOff, 1, 64),
        event(66150, T::NoteOn, 1, 62, 72),
        event(71662, T::ControlChange, 1, 21, 64),
        event(77175, T::NoteOff, 1, 62),
        event(77175, T::NoteOff, 0, 53),
        event(77175, T::NoteOff, 0, 60),
        event(77175, T::NoteOff, 0, 65),
        event(80000, T::ControlChange, 1, 11, 127),
        event(82750, T::ControlChange, 1, 21, 0),
        // Channel 0 stays audibly active while channel 1 prepares/replaces its
        // Program. The next part's CC120 (or final NoteOff) releases this note.
        event(82750, T::NoteOn, 0, 60, 64),
        event(kSegmentFrames, T::ProgramChange, 1, second ? initial : replacement),
    };
    for (auto& item : result) {
        if (item.type == T::NoteOn || item.type == T::NoteOff)
            item.data1 = static_cast<std::uint8_t>(item.data1 + transpose);
    }
    return result;
}

constexpr auto cleanup() {
    using T = realtime::EventType;
    return std::array{
        event(0, T::ControlChange, 0, 64, 0),
        event(0, T::ControlChange, 1, 64, 0),
        event(0, T::NoteOff, 0, 62), // part two's transposed held note
    };
}

inline constexpr auto kEventsPerSegment = segment(false, 0, 1).size();
inline constexpr auto kEvents = kEventsPerSegment * 2u + cleanup().size();
static_assert(kEventsPerSegment < realtime::RealtimeEventQueue::kCapacity);

constexpr std::uint64_t hashEvent(std::uint64_t hash, const realtime::RealtimeEvent& item) {
    // Canonical bytes, independent of struct padding and host endianness.
    for (unsigned shift = 0; shift < 64; shift += 8) {
        hash ^= static_cast<std::uint8_t>(item.timestamp_frame >> shift);
        hash *= kHashPrime;
    }
    for (auto byte : {static_cast<std::uint8_t>(item.type), item.channel, item.data1, item.data2,
                      static_cast<std::uint8_t>(item.value), static_cast<std::uint8_t>(item.value >> 8)}) {
        hash ^= byte; hash *= kHashPrime;
    }
    return hash;
}

constexpr std::uint64_t definitionHash(std::uint8_t initial, std::uint8_t replacement) {
    auto hash = kHashOffset;
    for (bool second : {false, true})
        for (const auto& item : segment(second, initial, replacement)) hash = hashEvent(hash, item);
    for (const auto& item : cleanup()) hash = hashEvent(hash, item);
    return hash;
}

} // namespace gm_synth::app::loop_realtime_replay
