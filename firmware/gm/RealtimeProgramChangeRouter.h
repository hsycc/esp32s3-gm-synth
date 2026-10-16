#pragma once

#include "RealtimeProgramChangeHandoff.h"
#include "ZoneResolver.h"

#include <cstdint>

namespace gm_synth::gm {

struct RealtimeProgramChangeRouterStats {
    std::uint64_t program_change_events = 0;
    std::uint64_t gm_reset_events = 0;
    std::uint64_t gm_reset_program_requests = 0;
    std::uint64_t requests_published = 0;
    std::uint64_t requests_rejected = 0;
};

// Thin audio-side router that adds Program Change / GM Reset handoff without
// pushing any SF2 parsing or storage work into RealtimeProgramEventApplier.
// Bank Select CCs are still handled by the wrapped applier. Program Change
// snapshots the current bank identity (except GM Channel 10, which maps to the
// reserved SoundFont percussion bank 128) and publishes only a compact request
// to control.
class RealtimeProgramChangeRouter {
public:
    RealtimeProgramChangeRouter(
        RealtimeProgramEventApplier& events,
        RealtimeProgramChangeHandoff& handoff)
        : events_(events), handoff_(handoff) {}

    bool apply(const realtime::RealtimeEvent& event, voice::VoicePool& voices);

    static bool callback(
        void* context,
        const realtime::RealtimeEvent& event,
        voice::VoicePool& voices);

    const RealtimeProgramChangeRouterStats& stats() const { return stats_; }

private:
    std::uint32_t allocateProgramToken();
    bool publishProgramRequest(std::uint8_t channel, std::uint8_t program);
    bool applyGmReset(voice::VoicePool& voices);

    RealtimeProgramEventApplier& events_;
    RealtimeProgramChangeHandoff& handoff_;
    std::uint32_t next_program_token_ = 1;
    RealtimeProgramChangeRouterStats stats_{};
};

}  // namespace gm_synth::gm
