#include "ZoneResolver.h"

#include "util/SaturatingCounter.h"

#include <algorithm>
#include <array>
#include <limits>

namespace gm_synth::gm {
namespace {

constexpr std::array<std::uint32_t, 12> kSemitoneRatioQ20{
    1048576u,
    1110928u,
    1176987u,
    1246974u,
    1321123u,
    1399681u,
    1482910u,
    1571089u,
    1664511u,
    1763488u,
    1868350u,
    1979448u,
};
constexpr std::uint32_t kRatioFractionBits = 20;
constexpr std::uint64_t kRatioRounding = std::uint64_t{1} << (kRatioFractionBits - 1u);

std::uint64_t multiplyRatioQ20(std::uint64_t value, std::uint32_t ratio_q20) {
    if (value == 0 || ratio_q20 == 0) {
        return 0;
    }
    const std::uint64_t max_product =
        std::numeric_limits<std::uint64_t>::max() - kRatioRounding;
    if (value > max_product / ratio_q20) {
        return std::numeric_limits<std::uint64_t>::max();
    }
    return (value * ratio_q20 + kRatioRounding) >> kRatioFractionBits;
}

}  // namespace

std::uint64_t ZoneResolver::transposeSemitones(
    std::uint64_t root_phase_increment_q32,
    int semitone_delta) {
    if (root_phase_increment_q32 == 0) {
        return 0;
    }

    int octaves = semitone_delta / 12;
    int remainder = semitone_delta % 12;
    if (remainder < 0) {
        remainder += 12;
        --octaves;
    }

    std::uint64_t value = multiplyRatioQ20(
        root_phase_increment_q32,
        kSemitoneRatioQ20[static_cast<std::size_t>(remainder)]);

    if (octaves > 0) {
        if (octaves >= 64 ||
            value > (std::numeric_limits<std::uint64_t>::max() >> octaves)) {
            return std::numeric_limits<std::uint64_t>::max();
        }
        value <<= octaves;
    } else if (octaves < 0) {
        const int shift = -octaves;
        if (shift >= 64) {
            return 1;
        }
        value >>= shift;
    }

    return std::max<std::uint64_t>(value, 1u);
}

ZoneResolveStatus ZoneResolver::resolveNoteOn(
    const PlaybackProgramSnapshot& program,
    std::uint8_t channel,
    std::uint8_t key,
    std::uint8_t velocity,
    std::uint32_t note_instance_id,
    ResolvedVoiceBatch* batch) const {
    if (batch == nullptr || channel >= 16 || key > 127 || velocity > 127 ||
        note_instance_id == 0 || program.channel != channel ||
        program.zone_count > PlaybackProgramSnapshot::kMaxZones ||
        (program.zone_count != 0 && program.zones == nullptr)) {
        return ZoneResolveStatus::InvalidArgument;
    }

    batch->clear();
    for (std::size_t i = 0; i < program.zone_count; ++i) {
        const PlaybackZone& zone = program.zones[i];
        if (!zone.matches(key, velocity)) {
            continue;
        }

        if (batch->count >= batch->voices.size()) {
            batch->clear();
            return ZoneResolveStatus::TooManyMatches;
        }

        voice::VoiceStartParams params{};
        params.sample_id = zone.sample_id;
        params.channel = channel;
        params.key = key;
        params.velocity = velocity;
        params.exclusive_class = zone.exclusive_class;
        params.note_instance_id = note_instance_id;
        params.phase_increment_q32 = zone.key_tracking
            ? transposeSemitones(
                  zone.root_phase_increment_q32,
                  static_cast<int>(key) - static_cast<int>(zone.root_key))
            : zone.root_phase_increment_q32;
        params.loop_mode = zone.loop_mode;
        params.gain_left_q15 = zone.gain_left_q15;
        params.gain_right_q15 = zone.gain_right_q15;
        params.envelope = zone.envelope;
        batch->voices[batch->count++] = params;
    }

    return batch->count == 0 ? ZoneResolveStatus::NoMatch : ZoneResolveStatus::Ok;
}

std::uint32_t RealtimeProgramEventApplier::allocateNoteInstanceId() {
    const std::uint32_t id = next_note_instance_id_;
    ++next_note_instance_id_;
    if (next_note_instance_id_ == 0) {
        next_note_instance_id_ = 1;
    }
    return id;
}

bool RealtimeProgramEventApplier::apply(
    const realtime::RealtimeEvent& event,
    voice::VoicePool& voices) {
    if (!event.valid() || event.data1 > 127 || event.data2 > 127) {
        util::saturatingIncrement(stats_.invalid_events);
        return false;
    }

    if (event.type == realtime::EventType::NoteOff ||
        (event.type == realtime::EventType::NoteOn && event.data2 == 0)) {
        util::saturatingIncrement(stats_.note_off_events);
        voices.noteOff(event.channel, event.data1);
        return true;
    }

    if (event.type != realtime::EventType::NoteOn) {
        util::saturatingIncrement(stats_.unsupported_events);
        return false;
    }

    util::saturatingIncrement(stats_.note_on_events);
    const PlaybackProgramSnapshot* program = programs_.active(event.channel);
    if (program == nullptr) {
        util::saturatingIncrement(stats_.missing_active_program);
        return false;
    }

    const ZoneResolveStatus status = resolver_.resolveNoteOn(
        *program,
        event.channel,
        event.data1,
        event.data2,
        allocateNoteInstanceId(),
        &batch_);

    if (status == ZoneResolveStatus::NoMatch) {
        util::saturatingIncrement(stats_.no_matching_zones);
        return true;
    }
    if (status == ZoneResolveStatus::TooManyMatches) {
        util::saturatingIncrement(stats_.resolve_overflows);
        return false;
    }
    if (status != ZoneResolveStatus::Ok) {
        util::saturatingIncrement(stats_.invalid_events);
        return false;
    }

    util::saturatingAdd(stats_.voices_resolved, static_cast<std::uint64_t>(batch_.count));
    bool all_started = true;
    for (std::size_t i = 0; i < batch_.count; ++i) {
        if (voices.start(batch_.voices[i])) {
            util::saturatingIncrement(stats_.voices_started);
        } else {
            util::saturatingIncrement(stats_.voice_start_failures);
            all_started = false;
        }
    }
    return all_started;
}

bool RealtimeProgramEventApplier::callback(
    void* context,
    const realtime::RealtimeEvent& event,
    voice::VoicePool& voices) {
    if (context == nullptr) {
        return false;
    }
    return static_cast<RealtimeProgramEventApplier*>(context)->apply(event, voices);
}

}  // namespace gm_synth::gm
