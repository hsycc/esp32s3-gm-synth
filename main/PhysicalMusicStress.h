#pragma once

#include "gm/PlaybackProgramSnapshot.h"
#include "gm/RealtimeProgramChangeHandoff.h"
#include "gm/RealtimeProgramChangeRouter.h"
#include "realtime/RealtimeEventQueue.h"
#include "realtime/ScheduledAudioBlockRenderer.h"
#include "sample/PublishedSampleDirectory.h"
#include "voice/VoiceRenderer.h"

#include <array>
#include <cstddef>
#include <cstdint>

namespace gm_synth::bringup {

// Physical-only production-shaped music stress harness. It keeps storage work
// out of the audio thread while exercising the same sample-accurate MIDI event,
// sustain, percussion, Program Change handoff, GM Reset, Voice and global-FX
// paths used by the runtime. The separate polyphony sweep establishes the
// highest consecutive full-FX voice count that remains inside one 64-frame
// render deadline on the actual MCU.
class PhysicalMusicStress {
public:
    explicit PhysicalMusicStress(sample::PublishedSampleDirectory& directory);

    bool initialize();

    static bool callback(
        void* context,
        const realtime::RealtimeEvent& event,
        voice::VoicePool& voices);

    bool runPolyphonySweep(
        voice::VoicePool& voices,
        realtime::ScheduledAudioBlockRenderer& renderer,
        std::uint64_t* block_start_frame);

    bool enqueueTimeline(
        realtime::RealtimeEventQueue& queue,
        voice::VoicePool& voices,
        const realtime::ScheduledAudioBlockRenderer& renderer,
        std::uint64_t block_start_frame,
        std::size_t measured_blocks);

    bool validateAndLog(
        realtime::RealtimeEventQueue& queue,
        const voice::VoicePool& voices,
        const realtime::ScheduledAudioBlockRenderer& renderer);

private:
    bool applyEvent(
        const realtime::RealtimeEvent& event,
        voice::VoicePool& voices);

    bool pushEvent(
        realtime::RealtimeEventQueue& queue,
        std::uint64_t frame,
        realtime::EventType type,
        std::uint8_t channel,
        std::uint8_t data1 = 0,
        std::uint8_t data2 = 0,
        std::uint16_t value = 0);

    sample::PublishedSampleDirectory& directory_;
    std::array<gm::PlaybackZone, 16> zones_{};
    std::array<gm::PlaybackProgramSnapshot, 16> programs_{};
    gm::PlaybackProgramSnapshotExchange program_exchange_{};
    gm::RealtimeProgramEventApplier event_applier_{program_exchange_};
    gm::RealtimeProgramChangeHandoff program_handoff_{};
    gm::RealtimeProgramChangeRouter program_router_{event_applier_, program_handoff_};

    voice::VoicePoolStats voice_stats_before_timeline_{};
    realtime::ScheduledAudioStats renderer_stats_before_timeline_{};
    realtime::RealtimeEventQueueStats queue_stats_before_timeline_{};

    std::size_t timeline_events_enqueued_ = 0;
    std::size_t timeline_peak_active_voices_ = 0;
    std::size_t max_stable_polyphony_ = 0;
    bool initialized_ = false;
};

}  // namespace gm_synth::bringup
