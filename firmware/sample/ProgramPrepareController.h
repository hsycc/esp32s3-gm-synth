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
    ResidencyCapacityFailed,
    ResidencyConflict,
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
    std::uint64_t capacity_failures = 0;
    std::uint64_t capacity_retries = 0;
    std::uint64_t cache_evictions = 0;
    std::uint64_t cache_evicted_bytes = 0;
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
//
// If the arena/metadata is full, Prepare may evict least-recently-used cache
// entries only when they are unpinned, unpublished and fully reclaimable. It
// never evicts Active/READY ownership to force a Program Change to fit.
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

    bool activateInitialProgram(
        std::uint8_t channel,
        std::uint16_t bank,
        std::uint8_t program,
        std::uint32_t token,
        const SampleId* dependencies,
        std::size_t dependency_count);

    bool prepareProgram(
        std::uint8_t channel,
        std::uint16_t bank,
        std::uint8_t program,
        std::uint32_t token,
        const SampleId* dependencies,
        std::size_t dependency_count);

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

    ProgramCommitAck acknowledgeAndRetire(std::uint8_t channel, std::uint32_t token);

    ProgramPrepareError lastError() const { return last_error_; }
    ProgramPrepareStats stats() const { return stats_; }

private:
    struct ChannelSnapshots {
        ProgramSnapshot a{};
        ProgramSnapshot b{};
    };

    static constexpr std::size_t kPublicationBitmapWords =
        (ProgramSnapshot::kMaxDependencies + 63u) / 64u;
    using PublicationBitmap = std::array<std::uint64_t, kPublicationBitmapWords>;

    bool prepareImpl(
        bool initial_activation,
        std::uint8_t channel,
        std::uint16_t bank,
        std::uint8_t program,
        std::uint32_t token,
        const SampleId* dependencies,
        std::size_t dependency_count);

    ResidentSampleView ensureResident(SampleId sample_id);
    bool evictOneSafeCacheSample();
    ProgramSnapshot* selectFreeSnapshot(std::uint8_t channel);
    bool ensurePublished(const ResidentSampleView& view, bool* newly_published);
    bool recordMatchesView(const ResidentRecord& record, const ResidentSampleView& view) const;
    void initializeRecord(ResidentRecord& record, const ResidentSampleView& view);
    void rollbackPinsAndPublications(
        const ProgramSnapshot& working_snapshot,
        const PublicationBitmap& newly_published,
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
