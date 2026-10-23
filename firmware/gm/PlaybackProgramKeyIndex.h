#pragma once

#include "PlaybackProgramSnapshot.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>

namespace gm_synth::gm {

constexpr std::size_t kPlaybackProgramIndexedZoneLimit = PlaybackProgramSnapshot::kMaxZones;
constexpr std::size_t kPlaybackProgramMidiKeyCount = PlaybackProgramSnapshot::kMidiKeyCount;

struct PlaybackProgramKeyIndexView {
    const PlaybackProgramKeySpan* spans = nullptr;
    const std::uint16_t* zone_indices = nullptr;
    std::uint32_t reference_count = 0;
    std::uint16_t zone_count = 0;

    bool valid() const {
        if (zone_count > kPlaybackProgramIndexedZoneLimit ||
            spans == nullptr ||
            (reference_count != 0 && zone_indices == nullptr)) {
            return false;
        }
        for (std::size_t key = 0; key < kPlaybackProgramMidiKeyCount; ++key) {
            const PlaybackProgramKeySpan& span = spans[key];
            if (span.begin > reference_count ||
                span.count > reference_count - span.begin) {
                return false;
            }
        }
        for (std::uint32_t index = 0; index < reference_count; ++index) {
            if (zone_indices[index] >= zone_count) {
                return false;
            }
        }
        return true;
    }

    PlaybackProgramKeySpan spanFor(std::uint8_t key) const {
        return key < kPlaybackProgramMidiKeyCount && spans != nullptr
            ? spans[key]
            : PlaybackProgramKeySpan{};
    }
};

enum class PlaybackProgramKeyIndexBuildStatus : std::uint8_t {
    Ok = 0,
    InvalidArgument,
    TooManyZones,
    InvalidZone,
    ReferenceCapacityExceeded,
};

struct PlaybackProgramKeyIndexBuildResult {
    PlaybackProgramKeyIndexBuildStatus status = PlaybackProgramKeyIndexBuildStatus::InvalidArgument;
    std::uint32_t required_references = 0;
    std::uint16_t max_candidates_per_key = 0;
};

// Build a stable, allocation-free key -> zone-index table outside the realtime
// domain. Each zone index is repeated once for every MIDI key in its key range.
// The audio path therefore scans only the candidate span for the incoming key
// and applies the existing velocity predicate to that bounded subset.
inline PlaybackProgramKeyIndexBuildResult buildPlaybackProgramKeyIndex(
    const PlaybackZone* zones,
    std::size_t zone_count,
    PlaybackProgramKeySpan* spans,
    std::uint16_t* zone_indices,
    std::size_t zone_index_capacity) {
    PlaybackProgramKeyIndexBuildResult result{};
    if ((zone_count != 0 && zones == nullptr) || spans == nullptr ||
        (zone_index_capacity != 0 && zone_indices == nullptr)) {
        return result;
    }
    if (zone_count > kPlaybackProgramIndexedZoneLimit ||
        zone_count > std::numeric_limits<std::uint16_t>::max()) {
        result.status = PlaybackProgramKeyIndexBuildStatus::TooManyZones;
        return result;
    }

    std::array<std::uint32_t, kPlaybackProgramMidiKeyCount> counts{};
    std::uint64_t required = 0;
    for (std::size_t zone_index = 0; zone_index < zone_count; ++zone_index) {
        const PlaybackZone& zone = zones[zone_index];
        if (zone.key_low > zone.key_high || zone.key_high >= kPlaybackProgramMidiKeyCount) {
            result.status = PlaybackProgramKeyIndexBuildStatus::InvalidZone;
            return result;
        }
        const std::uint32_t width =
            static_cast<std::uint32_t>(zone.key_high - zone.key_low) + 1u;
        required += width;
        if (required > std::numeric_limits<std::uint32_t>::max()) {
            result.status = PlaybackProgramKeyIndexBuildStatus::ReferenceCapacityExceeded;
            return result;
        }
        for (std::uint16_t key = zone.key_low; key <= zone.key_high; ++key) {
            ++counts[key];
        }
    }

    result.required_references = static_cast<std::uint32_t>(required);
    if (required > zone_index_capacity) {
        result.status = PlaybackProgramKeyIndexBuildStatus::ReferenceCapacityExceeded;
        return result;
    }

    std::uint32_t cursor = 0;
    std::array<std::uint32_t, kPlaybackProgramMidiKeyCount> write_cursor{};
    for (std::size_t key = 0; key < kPlaybackProgramMidiKeyCount; ++key) {
        if (counts[key] > std::numeric_limits<std::uint16_t>::max()) {
            result.status = PlaybackProgramKeyIndexBuildStatus::ReferenceCapacityExceeded;
            return result;
        }
        spans[key].begin = cursor;
        spans[key].count = static_cast<std::uint16_t>(counts[key]);
        write_cursor[key] = cursor;
        cursor += counts[key];
        if (counts[key] > result.max_candidates_per_key) {
            result.max_candidates_per_key = static_cast<std::uint16_t>(counts[key]);
        }
    }

    for (std::size_t zone_index = 0; zone_index < zone_count; ++zone_index) {
        const PlaybackZone& zone = zones[zone_index];
        for (std::uint16_t key = zone.key_low; key <= zone.key_high; ++key) {
            zone_indices[write_cursor[key]++] = static_cast<std::uint16_t>(zone_index);
        }
    }

    result.status = PlaybackProgramKeyIndexBuildStatus::Ok;
    return result;
}

}  // namespace gm_synth::gm
