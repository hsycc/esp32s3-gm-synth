#include "gm/PlaybackProgramKeyIndex.h"

#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <vector>

using gm_synth::gm::PlaybackProgramKeyIndexBuildStatus;
using gm_synth::gm::PlaybackProgramKeyIndexView;
using gm_synth::gm::PlaybackProgramKeySpan;
using gm_synth::gm::PlaybackZone;
using gm_synth::gm::buildPlaybackProgramKeyIndex;
using gm_synth::gm::kPlaybackProgramIndexedZoneLimit;
using gm_synth::gm::kPlaybackProgramMidiKeyCount;

namespace {

void testStableCandidateSpans() {
    std::array<PlaybackZone, 3> zones{};
    zones[0].key_low = 0;
    zones[0].key_high = 10;
    zones[1].key_low = 5;
    zones[1].key_high = 5;
    zones[2].key_low = 5;
    zones[2].key_high = 20;

    std::array<PlaybackProgramKeySpan, kPlaybackProgramMidiKeyCount> spans{};
    std::array<std::uint16_t, 32> refs{};
    const auto result = buildPlaybackProgramKeyIndex(
        zones.data(), zones.size(), spans.data(), refs.data(), refs.size());
    assert(result.status == PlaybackProgramKeyIndexBuildStatus::Ok);
    assert(result.required_references == 28);
    assert(result.max_candidates_per_key == 3);

    PlaybackProgramKeyIndexView view{
        spans.data(), refs.data(), result.required_references,
        static_cast<std::uint16_t>(zones.size())};
    assert(view.valid());

    const PlaybackProgramKeySpan key0 = view.spanFor(0);
    assert(key0.count == 1);
    assert(refs[key0.begin] == 0);

    const PlaybackProgramKeySpan key5 = view.spanFor(5);
    assert(key5.count == 3);
    assert(refs[key5.begin + 0] == 0);
    assert(refs[key5.begin + 1] == 1);
    assert(refs[key5.begin + 2] == 2);

    const PlaybackProgramKeySpan key11 = view.spanFor(11);
    assert(key11.count == 1);
    assert(refs[key11.begin] == 2);
}

void testCapacityQueryAndLargeProgram() {
    std::vector<PlaybackZone> zones(720);
    for (std::size_t index = 0; index < zones.size(); ++index) {
        const std::uint8_t key = static_cast<std::uint8_t>(index % 128u);
        zones[index].key_low = key;
        zones[index].key_high = key;
    }

    std::array<PlaybackProgramKeySpan, kPlaybackProgramMidiKeyCount> spans{};
    auto query = buildPlaybackProgramKeyIndex(
        zones.data(), zones.size(), spans.data(), nullptr, 0);
    assert(query.status == PlaybackProgramKeyIndexBuildStatus::ReferenceCapacityExceeded);
    assert(query.required_references == 720);
    assert(query.max_candidates_per_key == 0);  // capacity query exits before prefix materialization

    std::vector<std::uint16_t> refs(query.required_references);
    const auto built = buildPlaybackProgramKeyIndex(
        zones.data(), zones.size(), spans.data(), refs.data(), refs.size());
    assert(built.status == PlaybackProgramKeyIndexBuildStatus::Ok);
    assert(built.required_references == 720);
    assert(built.max_candidates_per_key == 6);

    PlaybackProgramKeyIndexView view{
        spans.data(), refs.data(), built.required_references,
        static_cast<std::uint16_t>(zones.size())};
    assert(view.valid());
    for (std::uint8_t key = 0; key < 128; ++key) {
        const auto span = view.spanFor(key);
        assert(span.count == (key < 80 ? 6 : 5));
        for (std::size_t offset = 0; offset < span.count; ++offset) {
            assert(zones[refs[span.begin + offset]].key_low == key);
        }
    }
}

void testSafetyBounds() {
    std::array<PlaybackProgramKeySpan, kPlaybackProgramMidiKeyCount> spans{};
    std::array<std::uint16_t, 8> refs{};

    PlaybackZone invalid{};
    invalid.key_low = 100;
    invalid.key_high = 99;
    const auto invalid_result = buildPlaybackProgramKeyIndex(
        &invalid, 1, spans.data(), refs.data(), refs.size());
    assert(invalid_result.status == PlaybackProgramKeyIndexBuildStatus::InvalidZone);

    std::vector<PlaybackZone> too_many(kPlaybackProgramIndexedZoneLimit + 1u);
    const auto too_many_result = buildPlaybackProgramKeyIndex(
        too_many.data(), too_many.size(), spans.data(), refs.data(), refs.size());
    assert(too_many_result.status == PlaybackProgramKeyIndexBuildStatus::TooManyZones);
}

}  // namespace

int main() {
    testStableCandidateSpans();
    testCapacityQueryAndLargeProgram();
    testSafetyBounds();
    std::cout << "Playback program key-index checks passed\n";
    return 0;
}
