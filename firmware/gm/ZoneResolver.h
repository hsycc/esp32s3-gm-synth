#pragma once

#include "MidiChannelState.h"
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

    // BEGIN V1_APP_LOOP
#if GM_SYNTH_EXPERIMENTAL_VOICE_LOOP
    // Exact prepared-zone identity, not an inferred sample/geometry match.
    std::array<std::uint16_t, kCapacity> zone_indices{};
#endif
    // END V1_APP_LOOP
    void clear() { count = 0; }
};

// Realtime-safe resolver over immutable prepared playback metadata. It does no
// allocation, storage I/O, SF2 parsing, logarithms or floating-point pitch
// conversion. Legacy snapshots scan at most kLinearScanZoneLimit zones;
// indexed snapshots scan only the immutable candidate span for the incoming key.
class ZoneResolver {
public:
    // Compatibility path used by existing direct tests/callers. It applies only
    // prepared zone state, exactly as before MIDI channel-state integration.
    ZoneResolveStatus resolveNoteOn(
        const PlaybackProgramSnapshot& program,
        std::uint8_t channel,
        std::uint8_t key,
        std::uint8_t velocity,
        std::uint32_t note_instance_id,
        ResolvedVoiceBatch* batch) const;

    // Production path: after raw zone selection, apply effective velocity plus
    // current CC1/7/10/11/91/93, Channel Pressure and Pitch Bend/RPN state
    // before Voice::start(). It also carries immutable source values needed to
    // refresh a sounding Voice when those channel controls later change.
    ZoneResolveStatus resolveNoteOn(
        const PlaybackProgramSnapshot& program,
        const MidiChannelState& channel_state,
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

    // Fractional-cent companion used for channel Pitch Bend. The implementation
    // uses a 100-entry Q20 cent-ratio table plus the semitone path above.
    static std::uint64_t transposeCents(
        std::uint64_t root_phase_increment_q32,
        int cents_delta);

    static int pitchBendCents(const MidiChannelState& state);
};

struct RealtimeProgramEventStats {
    std::uint64_t note_on_events = 0;
    std::uint64_t note_off_events = 0;
    std::uint64_t control_change_events = 0;
    std::uint64_t pitch_bend_events = 0;
    std::uint64_t channel_pressure_events = 0;
    std::uint64_t controller_state_changes = 0;
    std::uint64_t filter_controller_changes = 0;
    std::uint64_t active_voice_updates = 0;
    std::uint64_t sustain_releases = 0;
    std::uint64_t all_notes_off_events = 0;
    std::uint64_t all_sound_off_events = 0;
    std::uint64_t unsupported_events = 0;
    std::uint64_t invalid_events = 0;
    std::uint64_t missing_active_program = 0;
    // GM Channel 10 is never allowed to fall through to an old melodic bank.
    // While bank-128 percussion is unavailable/pending, NoteOn is safely silent.
    std::uint64_t percussion_program_unavailable = 0;
    std::uint64_t no_matching_zones = 0;
    std::uint64_t resolve_overflows = 0;
    std::uint64_t voices_resolved = 0;
    std::uint64_t voices_started = 0;
    std::uint64_t voice_start_failures = 0;
};

// Physical-only attribution of the densest NoteOn event block. This is
// deliberately disabled by default to preserve ordinary realtime cost.
struct NoteOnBurstTiming {
    std::uint64_t block_start_frame = 0;
    std::uint32_t note_on_count = 0;
    std::uint32_t zone_resolve_us = 0;
    std::uint32_t voice_start_us = 0;
    std::uint32_t max_zone_resolve_us = 0;
    std::uint32_t max_voice_start_us = 0;
};

// Production-shaped event adapter. MIDI channel state is fixed-size and owned
// by the audio-side applier, so sample-accurate channel events update both the
// state observed by future NoteOn events and already-sounding Voices.
class RealtimeProgramEventApplier {
public:
    explicit RealtimeProgramEventApplier(PlaybackProgramSnapshotExchange& programs)
        : programs_(programs) {}

    // BEGIN V1_APP_LOOP
#if GM_SYNTH_EXPERIMENTAL_VOICE_LOOP
    using ExperimentalVoiceStarter = bool (*)(
        void*, const PlaybackProgramSnapshot&, std::uint16_t,
        const realtime::RealtimeEvent&, const voice::VoiceStartParams&, voice::VoicePool&);
    // Configure only before audio starts, or after it is quiescent. The callback
    // consumes borrowed plans synchronously; it must not mutate channel state.
    void setExperimentalVoiceStarter(ExperimentalVoiceStarter start, void* context) {
        experimental_starter_ = start;
        experimental_context_ = context;
    }
#endif
    // END V1_APP_LOOP
    bool apply(const realtime::RealtimeEvent& event, voice::VoicePool& voices);

    static bool callback(
        void* context,
        const realtime::RealtimeEvent& event,
        voice::VoicePool& voices);

    const RealtimeProgramEventStats& stats() const { return stats_; }
    void enableNoteOnBurstTiming() {
        note_on_burst_timing_enabled_ = true;
        note_on_burst_current_ = NoteOnBurstTiming{};
        note_on_burst_worst_ = NoteOnBurstTiming{};
    }
    NoteOnBurstTiming noteOnBurstTiming() const {
        return note_on_burst_current_.note_on_count >
                   note_on_burst_worst_.note_on_count
            ? note_on_burst_current_ : note_on_burst_worst_;
    }
    const MidiChannelState& channelState(std::uint8_t channel) const {
        return channel_states_.channel(channel);
    }
    MidiChannelStateBank& channelStates() { return channel_states_; }

private:
    std::uint32_t allocateNoteInstanceId();
    std::size_t refreshActiveVoices(std::uint8_t channel, voice::VoicePool& voices);

    // BEGIN V1_APP_LOOP
#if GM_SYNTH_EXPERIMENTAL_VOICE_LOOP
    ExperimentalVoiceStarter experimental_starter_ = nullptr;
    void* experimental_context_ = nullptr;
#endif
    // END V1_APP_LOOP
    PlaybackProgramSnapshotExchange& programs_;
    MidiChannelStateBank channel_states_{};
    ZoneResolver resolver_{};
    ResolvedVoiceBatch batch_{};
    std::uint32_t next_note_instance_id_ = 1;
    RealtimeProgramEventStats stats_{};
    bool note_on_burst_timing_enabled_ = false;
    NoteOnBurstTiming note_on_burst_current_{};
    NoteOnBurstTiming note_on_burst_worst_{};
};

}  // namespace gm_synth::gm
