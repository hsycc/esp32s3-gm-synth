#pragma once

#include "PlaybackProgramKeyIndex.h"
#include "PlaybackProgramSnapshot.h"
#include "ProgramChangeMailbox.h"
#include "RealtimeProgramChangeHandoff.h"
#include "sample/ProgramPrepareController.h"
#include "sf2/Sf2PlaybackProgramPreparer.h"

#include <cstddef>
#include <cstdint>

namespace gm_synth::gm {

enum class ProgramChangeServiceStatus : std::uint8_t {
    Ok = 0,
    NoPending,
    TransitionBusy,
    InvalidWorkspace,
    NoActiveResidencySnapshot,
    PresetUnavailable,
    PlaybackPrepareFailed,
    ResidencyPrepareFailed,
    PlaybackPublishInvariantFailed,
    CommitNotReady,
    AcknowledgeFailed,
};

// Caller-owned storage for one prepared playback generation. The object and its
// playback_zones/key-index buffers must remain alive and immutable while the
// snapshot is READY, Active or Retired. In practice keep two of these per MIDI
// channel and reuse a retired one only after acknowledgeCommitted() returns it.
struct ProgramChangePrepareWorkspace {
    sf2::NativeResolvedZone* native_scratch = nullptr;
    std::size_t native_capacity = 0;
    sf2::ResolvedZoneModulation* modulation_scratch = nullptr;
    std::size_t modulation_capacity = 0;
    PlaybackZone* playback_zones = nullptr;
    std::size_t playback_capacity = 0;
    PlaybackProgramKeySpan* key_spans = nullptr;
    std::uint16_t* key_zone_indices = nullptr;
    std::size_t key_zone_index_capacity = 0;
    sample::SampleId* dependencies = nullptr;
    std::size_t dependency_capacity = 0;
    sf2::PlaybackPreparePolicy policy = sf2::PlaybackPreparePolicy::AllowPreview;
    PlaybackProgramSnapshot snapshot{};

    bool structurallyValid() const {
        const bool has_any_key_index_storage =
            key_spans != nullptr || key_zone_indices != nullptr || key_zone_index_capacity != 0;
        const bool key_index_storage_valid =
            !has_any_key_index_storage ||
            (key_spans != nullptr &&
             (key_zone_index_capacity == 0 || key_zone_indices != nullptr));
        return native_scratch != nullptr && native_capacity != 0 &&
               modulation_scratch != nullptr && modulation_capacity != 0 &&
               playback_zones != nullptr && playback_capacity != 0 &&
               dependencies != nullptr && dependency_capacity != 0 &&
               key_index_storage_valid;
    }
};

struct ProgramChangeServiceResult {
    ProgramChangeServiceStatus status = ProgramChangeServiceStatus::NoPending;
    ProgramChangeRequest request{};
    sf2::PlaybackPrepareResult playback_prepare{};
    sample::ProgramPrepareError residency_error = sample::ProgramPrepareError::None;

    bool ok() const { return status == ProgramChangeServiceStatus::Ok; }
};

struct ProgramChangeRetireAck {
    ProgramChangeServiceStatus status = ProgramChangeServiceStatus::AcknowledgeFailed;
    std::uint32_t token = 0;
    const PlaybackProgramSnapshot* retired_playback = nullptr;

    bool ok() const { return status == ProgramChangeServiceStatus::Ok; }
};

struct ProgramChangeControlServiceStats {
    std::uint64_t realtime_requests_drained = 0;
    std::uint64_t service_attempts = 0;
    std::uint64_t service_successes = 0;
    std::uint64_t busy_deferrals = 0;
    std::uint64_t preset_unavailable_drops = 0;
    std::uint64_t playback_prepare_failures = 0;
    std::uint64_t residency_prepare_failures = 0;
    std::uint64_t publish_invariant_failures = 0;
    std::uint64_t commit_acks = 0;
    std::uint64_t ack_failures = 0;
};

// Control/storage-domain Program Change transaction coordinator.
//
// The realtime handoff is drained into the existing control-only mailbox. For a
// pending request this service first materializes immutable playback metadata,
// then prepares/pins residency from the exact dependency list, and only then
// publishes playback READY. Audio can therefore commit only a matching pair via
// ProgramRuntimeCommitCoordinator. No parser, allocation or storage I/O occurs
// on the audio thread.
class ProgramChangeControlService {
public:
    ProgramChangeControlService(
        RealtimeProgramChangeHandoff& realtime_handoff,
        ProgramChangeMailbox& mailbox,
        sf2::Sf2PdtaReader& pdta,
        const sf2::Sf2PlaybackProgramPreparer& playback_preparer,
        sample::ProgramPrepareController& residency_preparer,
        sample::ProgramSnapshotExchange& residency_exchange,
        PlaybackProgramSnapshotExchange& playback_exchange)
        : realtime_handoff_(realtime_handoff),
          mailbox_(mailbox),
          pdta_(pdta),
          playback_preparer_(playback_preparer),
          residency_preparer_(residency_preparer),
          residency_exchange_(residency_exchange),
          playback_exchange_(playback_exchange) {}

    std::size_t drainRealtimeRequests();

    ProgramChangeServiceResult serviceChannel(
        std::uint8_t channel,
        ProgramChangePrepareWorkspace& workspace);

    ProgramChangeRetireAck acknowledgeCommitted(std::uint8_t channel);

    const ProgramChangeControlServiceStats& stats() const { return stats_; }

private:
    bool workspaceIsFree(
        std::uint8_t channel,
        const ProgramChangePrepareWorkspace& workspace) const;

    RealtimeProgramChangeHandoff& realtime_handoff_;
    ProgramChangeMailbox& mailbox_;
    sf2::Sf2PdtaReader& pdta_;
    const sf2::Sf2PlaybackProgramPreparer& playback_preparer_;
    sample::ProgramPrepareController& residency_preparer_;
    sample::ProgramSnapshotExchange& residency_exchange_;
    PlaybackProgramSnapshotExchange& playback_exchange_;
    ProgramChangeControlServiceStats stats_{};
};

}  // namespace gm_synth::gm
