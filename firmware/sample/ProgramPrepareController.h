#pragma once

#include "ProgramSnapshot.h"
#include "PublishedSampleDirectory.h"
#include "SampleResidencyManager.h"

#include <array>
#include <cstddef>
#include <cstdint>

namespace gm_synth::sample {

enum class ProgramPrepareError : std::uint8_t {
    None = 0,
    InvalidArgument,
    TransitionBusy,
    NotResident,
    PinFailed,
    PublicationConflict,
    RetiringGenerationBusy,
    SnapshotUnavailable,
    ReadyPublishFailed,
    InitialActivationFailed,
    RetireFailed,
};

struct ProgramPrepareStats {
    std::uint64_t prepare_attempts = 0;
    std::uint64_t prepare_successes = 0;
    std::uint64_t prepare_failures = 0;
    std::uint64_t samples_pinned = 0;
    std::uint64_t samples_published = 0;
    std::uint64_t samples_reused_published = 0;
    std::uint64_t retire_acks = 0;
    std::uint64_t retired_pins = 0;
    std::uint64_t samples_unpublished_after_retire = 0;
};

// Control/storage-domain transaction coordinator for M3 Phase B.
//
// This first version intentionally prepares only samples already resident in
// SampleResidencyManager. Missing-sample storage loading is added in the next
// step; the transactional pin/publish/READY/rollback semantics stay the same.
class ProgramPrepareController {
public:
    ProgramPrepareController(
        SampleResidencyManager& residency,
        PublishedSampleDirectory& directory,
        ProgramSnapshotExchange& exchange);

    bool setBankGeneration(std::uint32_t bank_generation);

    // Startup path: transactionally pin/publish dependencies and install the
    // snapshot directly as Active before realtime playback begins.
    bool activateInitialResidentProgram(
        std::uint8_t channel,
        std::uint16_t bank,
        std::uint8_t program,
        std::uint32_t token,
        const SampleId* dependencies,
        std::size_t dependency_count);

    // Runtime path: transactionally pin/publish dependencies, build an
    // immutable snapshot and publish it as READY. Audio later commits it at a
    // block boundary through ProgramSnapshotExchange.
    bool prepareResidentProgram(
        std::uint8_t channel,
        std::uint16_t bank,
        std::uint8_t program,
        std::uint32_t token,
        const SampleId* dependencies,
        std::size_t dependency_count);

    // Control-side commit acknowledgement + old-snapshot pin retirement.
    // Returns the exchange acknowledgement. If retirement fails, lastError()
    // reports RetireFailed and the returned ack remains valid for diagnostics.
    ProgramCommitAck acknowledgeAndRetire(std::uint8_t channel, std::uint32_t token);

    ProgramPrepareError lastError() const { return last_error_; }
    ProgramPrepareStats stats() const { return stats_; }

private:
    struct ChannelSnapshots {
        ProgramSnapshot a{};
        ProgramSnapshot b{};
    };

    bool prepareImpl(
        bool initial_activation,
        std::uint8_t channel,
        std::uint16_t bank,
        std::uint8_t program,
        std::uint32_t token,
        const SampleId* dependencies,
        std::size_t dependency_count);

    ProgramSnapshot* selectFreeSnapshot(std::uint8_t channel);
    bool ensurePublished(const ResidentSampleView& view, bool* newly_published);
    bool recordMatchesView(const ResidentRecord& record, const ResidentSampleView& view) const;
    void initializeRecord(ResidentRecord& record, const ResidentSampleView& view);
    void rollbackPinsAndPublications(
        const std::array<SampleId, ProgramSnapshot::kMaxDependencies>& ids,
        const std::array<bool, ProgramSnapshot::kMaxDependencies>& newly_published,
        std::size_t count);
    bool retireSnapshot(const ProgramSnapshot& snapshot);
    bool dependencyListIsValid(const SampleId* dependencies, std::size_t dependency_count) const;
    void setError(ProgramPrepareError error) { last_error_ = error; }

    SampleResidencyManager& residency_;
    PublishedSampleDirectory& directory_;
    ProgramSnapshotExchange& exchange_;

    std::array<ResidentRecord, SampleResidencyManager::kMaxResidentSamples> records_{};
    std::array<ChannelSnapshots, ProgramSnapshotExchange::kMidiChannels> snapshots_{};

    std::uint32_t bank_generation_ = 1;
    ProgramPrepareError last_error_ = ProgramPrepareError::None;
    ProgramPrepareStats stats_{};
};

}  // namespace gm_synth::sample
