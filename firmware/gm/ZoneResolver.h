#pragma once

#include "PlaybackProgramSnapshot.h"
#include "realtime/RealtimeEventQueue.h"

#include <array>
#include <cstddef>
#include <cstdint>

namespace gm_synth::gm {

enum class ZoneResolveStatus : std::uint8_t {
    Ok = 0,
    NoMatch,
    InvalidArgument,
    TooManyMatches,
};

struct ResolvedVoiceBatch {
    static constexpr std::size_t kCapacity = voice::VoicePool::kHardVoiceCap;

    std::size_t count = 0;
    std::array<voice::VoiceStartParams, kCapacity> voices{};

    void clear() { count = 0; }
};

// Realtime-safe resolver over immutable prepared playback metadata. It does no
// allocation, storage I/O, SF2 parsing, logarithms or floating-point pitch
// conversion. The scan is bounded by PlaybackProgramSnapshot::kMaxZones.
class ZoneResolver {
public:
    ZoneResolveStatus resolveNoteOn(
        const PlaybackProgramSnapshot& program,
        std::uint8_t channel,
        std::uint8_t key,
        std::uint8_t velocity,
        std::uint32_t note_instance_id,
        ResolvedVoiceBatch* batch) const;

    // Equal-temperament integer transpose used by prepared zones. Fine/coarse
    // tuning is baked into the zone's root increment by the control domain.
    static std::uint64_t transposeSemitones(
        std::uint64_t root_phase_increment_q32,
        int semitone_delta);
};

struct RealtimeProgramEventStats {
    std::uint64_t note_on_events = 0;
    std::uint64_t note_off_events = 0;
    std::uint64_t unsupported_events = 0;
    std::uint64_t invalid_events = 0;
    std::uint64_t missing_active_program = 0;
    std::uint64_t no_matching_zones = 0;
    std::uint64_t resolve_overflows = 0;
    std::uint64_t voices_resolved = 0;
    std::uint64_t voices_started = 0;
    std::uint64_t voice_start_failures = 0;
};

// Production-shaped M6 event adapter: raw NoteOn/NoteOff -> active immutable
// playback snapshot -> resolved VoiceStartParams -> VoicePool. All layers from
// one logical NoteOn receive the same non-zero note_instance_id.
class RealtimeProgramEventApplier {
public:
    explicit RealtimeProgramEventApplier(PlaybackProgramSnapshotExchange& programs)
        : programs_(programs) {}

    bool apply(const realtime::RealtimeEvent& event, voice::VoicePool& voices);

    static bool callback(
        void* context,
        const realtime::RealtimeEvent& event,
        voice::VoicePool& voices);

    const RealtimeProgramEventStats& stats() const { return stats_; }

private:
    std::uint32_t allocateNoteInstanceId();

    PlaybackProgramSnapshotExchange& programs_;
    ZoneResolver resolver_{};
    ResolvedVoiceBatch batch_{};
    std::uint32_t next_note_instance_id_ = 1;
    RealtimeProgramEventStats stats_{};
};

}  // namespace gm_synth::gm
