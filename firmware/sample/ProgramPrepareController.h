#pragma once

#include "ProgramSnapshot.h"
#include "PublishedSampleDirectory.h"
#include "SampleResidencyManager.h"
#include "SampleSource.h"

#include <array>
#include <cstddef>
#include <cstdint>

namespace gm_synth::sample {

enum class ProgramPrepareError : std::uint8_t {
    None = 0,
    InvalidArgument,
    TransitionBusy,
    NotResident,
    SourceDescriptorFailed,
    SourceLoadFailed,
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
    std::uint64_t samples_loaded = 0;
    std::uint64_t sample_bytes_loaded = 0;
    std::uint64_t samples_reused_resident = 0;
    std::uint64_t source_failures = 0;
    std::uint64_t samples_pinned = 0;
    std::uint64_t samples_published = 0;
    std::uint64_t samples_reused_published = 0;
    std::uint64_t retire_acks = 0;
    std::uint64_t retired_pins = 0;
    std::uint64_t samples_unpublished_after_retire = 0;
};

// Control/storage-domain transaction coordinator for M3 Phase B.
//
// Missing samples may be loaded synchronously on the control/storage task via
// SampleSource -> SampleResidencyManager::loadFromSource(). No source method is
// ever called from Audio. Successfully loaded samples remain as unpinned cache
// entries if a later dependency causes the prepare transaction to fail.
class ProgramPrepareController {
public:
    ProgramPrepareController(
        SampleResidencyManager& residency,
        PublishedSampleDirectory& directory,
        ProgramSnapshotExchange& exchange,
        SampleSource* source = nullptr);

    void setSampleSource(SampleSource* source) { source_ = source; }
    SampleSource* sampleSource() const { return source_; }

    bool setBankGeneration(std::uint32_t bank_generation);

    // General startup path. Missing dependencies may be loaded from source_.
    bool activateInitialProgram(
        std::uint8_t channel,
        std::uint16_t bank,
        std::uint8_t program,
        std::uint32_t token,
        const SampleId* dependencies,
        std::size_t dependency_count);

    // General runtime path. Missing dependencies may be loaded from source_.
    bool prepareProgram(
        std::uint8_t channel,
        std::uint16_t bank,
        std::uint8_t program,
        std::uint32_t token,
        const SampleId* dependencies,
        std::size_t dependency_count);

    // Backward-compatible resident-only naming. With no source configured these
    // retain the old behavior: a missing sample fails with NotResident.
    bool activateInitialResidentProgram(
        std::uint8_t channel,
        std::uint16_t bank,
        std::uint8_t program,
        std::uint32_t token,
        const SampleId* dependencies,
        std::size_t dependency_count) {
        return activateInitialProgram(
            channel, bank, program, token, dependencies, dependency_count);
    }

    bool prepareResidentProgram(
        std::uint8_t channel,
        std::uint16_t bank,
        std::uint8_t program,
        std::uint32_t token,
        const SampleId* dependencies,
        std::size_t dependency_count) {
        return prepareProgram(channel, bank, program, token, dependencies, dependency_count);
    }

    // Control-side commit acknowledgement + old-snapshot pin retirement.
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

    ResidentSampleView ensureResident(SampleId sample_id);
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
    SampleSource* source_ = nullptr;

    std::array<ResidentRecord, SampleResidencyManager::kMaxResidentSamples> records_{};
    std::array<ChannelSnapshots, ProgramSnapshotExchange::kMidiChannels> snapshots_{};

    std::uint32_t bank_generation_ = 1;
    ProgramPrepareError last_error_ = ProgramPrepareError::None;
    ProgramPrepareStats stats_{};
};

}  // namespace gm_synth::sample
