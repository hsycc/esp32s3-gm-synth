from pathlib import Path

path = Path("firmware/gm/ZoneResolver.cpp")
text = path.read_text(encoding="utf-8")

old_include = '#include "ZoneResolver.h"\n\n#include "sf2/Sf2FilterMath.h"'
new_include = '#include "ZoneResolver.h"\n\n#include "PlaybackProgramKeyIndex.h"\n#include "sf2/Sf2FilterMath.h"'
if text.count(old_include) != 1:
    raise SystemExit("unexpected ZoneResolver include preamble")
text = text.replace(old_include, new_include, 1)

old_validation = '''bool validResolveArguments(
    const PlaybackProgramSnapshot& program,
    std::uint8_t channel,
    std::uint8_t key,
    std::uint8_t velocity,
    std::uint32_t note_instance_id,
    const ResolvedVoiceBatch* batch) {
    return batch != nullptr && channel < 16 && key <= 127 && velocity <= 127 &&
        note_instance_id != 0 && program.channel == channel &&
        program.zone_count <= PlaybackProgramSnapshot::kMaxZones &&
        (program.zone_count == 0 || program.zones != nullptr);
}
'''
new_validation = '''bool validResolveArguments(
    const PlaybackProgramSnapshot& program,
    std::uint8_t channel,
    std::uint8_t key,
    std::uint8_t velocity,
    std::uint32_t note_instance_id,
    const ResolvedVoiceBatch* batch) {
    return batch != nullptr && channel < 16 && key <= 127 && velocity <= 127 &&
        note_instance_id != 0 && program.channel == channel &&
        program.zone_count <= PlaybackProgramSnapshot::kMaxZones &&
        (program.zone_count == 0 || program.zones != nullptr);
}

struct ZoneCandidateSpan {
    const std::uint16_t* zone_indices = nullptr;
    std::size_t count = 0;
    bool indexed = false;
};

bool noteOnCandidates(
    const PlaybackProgramSnapshot& program,
    std::uint8_t key,
    ZoneCandidateSpan* output) {
    if (output == nullptr || key >= PlaybackProgramSnapshot::kMidiKeyCount) {
        return false;
    }

    const bool has_any_index_state =
        program.key_spans != nullptr || program.key_zone_indices != nullptr ||
        program.key_reference_count != 0;
    if (!has_any_index_state) {
        if (program.zone_count > PlaybackProgramSnapshot::kLinearScanZoneLimit) {
            return false;
        }
        output->zone_indices = nullptr;
        output->count = program.zone_count;
        output->indexed = false;
        return true;
    }

    if (program.key_spans == nullptr ||
        (program.key_reference_count != 0 && program.key_zone_indices == nullptr)) {
        return false;
    }
    const PlaybackProgramKeySpan span = program.key_spans[key];
    if (span.begin > program.key_reference_count ||
        span.count > program.key_reference_count - span.begin) {
        return false;
    }

    output->zone_indices = span.count == 0
        ? nullptr
        : program.key_zone_indices + span.begin;
    output->count = span.count;
    output->indexed = true;
    return true;
}

bool candidateZoneIndex(
    const PlaybackProgramSnapshot& program,
    const ZoneCandidateSpan& candidates,
    std::size_t candidate,
    std::size_t* zone_index) {
    if (zone_index == nullptr || candidate >= candidates.count) {
        return false;
    }
    const std::size_t resolved = candidates.indexed
        ? static_cast<std::size_t>(candidates.zone_indices[candidate])
        : candidate;
    if (resolved >= program.zone_count) {
        return false;
    }
    *zone_index = resolved;
    return true;
}
'''
if text.count(old_validation) != 1:
    raise SystemExit("unexpected validResolveArguments body")
text = text.replace(old_validation, new_validation, 1)

old_first = '''    if (!validResolveArguments(program, channel, key, velocity, note_instance_id, batch)) {
        return ZoneResolveStatus::InvalidArgument;
    }

    batch->clear();
    for (std::size_t i = 0; i < program.zone_count; ++i) {
        const PlaybackZone& zone = program.zones[i];
'''
new_first = '''    ZoneCandidateSpan candidates{};
    if (!validResolveArguments(program, channel, key, velocity, note_instance_id, batch) ||
        !noteOnCandidates(program, key, &candidates)) {
        return ZoneResolveStatus::InvalidArgument;
    }

    batch->clear();
    for (std::size_t candidate = 0; candidate < candidates.count; ++candidate) {
        std::size_t i = 0;
        if (!candidateZoneIndex(program, candidates, candidate, &i)) {
            batch->clear();
            return ZoneResolveStatus::InvalidArgument;
        }
        const PlaybackZone& zone = program.zones[i];
'''
if text.count(old_first) != 1:
    raise SystemExit("unexpected compatibility resolve loop")
text = text.replace(old_first, new_first, 1)

old_second = '''    if (!validResolveArguments(program, channel, key, velocity, note_instance_id, batch)) {
        return ZoneResolveStatus::InvalidArgument;
    }

    const int bend_cents = pitchBendCents(channel_state);
    batch->clear();
    for (std::size_t i = 0; i < program.zone_count; ++i) {
        const PlaybackZone& zone = program.zones[i];
'''
new_second = '''    ZoneCandidateSpan candidates{};
    if (!validResolveArguments(program, channel, key, velocity, note_instance_id, batch) ||
        !noteOnCandidates(program, key, &candidates)) {
        return ZoneResolveStatus::InvalidArgument;
    }

    const int bend_cents = pitchBendCents(channel_state);
    batch->clear();
    for (std::size_t candidate = 0; candidate < candidates.count; ++candidate) {
        std::size_t i = 0;
        if (!candidateZoneIndex(program, candidates, candidate, &i)) {
            batch->clear();
            return ZoneResolveStatus::InvalidArgument;
        }
        const PlaybackZone& zone = program.zones[i];
'''
if text.count(old_second) != 1:
    raise SystemExit("unexpected production resolve loop")
text = text.replace(old_second, new_second, 1)

path.write_text(text, encoding="utf-8")
