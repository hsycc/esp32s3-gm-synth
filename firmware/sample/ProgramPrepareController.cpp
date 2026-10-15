#include "ProgramPrepareController.h"

#include <limits>

namespace gm_synth::sample {
namespace {

struct SourceFillContext {
    SampleSource* source = nullptr;
    SampleId sample_id = 0;
};

bool fillFromSampleSource(void* opaque, std::uint8_t* destination, std::size_t pcm_bytes) {
    auto& context = *static_cast<SourceFillContext*>(opaque);
    return context.source != nullptr &&
           context.source->fillSample(context.sample_id, destination, pcm_bytes);
}

bool isCapacityError(ResidencyError error) {
    return error == ResidencyError::NoContiguousSpace ||
           error == ResidencyError::MetadataFull;
}

}  // namespace

ProgramPrepareController::ProgramPrepareController(
    SampleResidencyManager& residency,
    PublishedSampleDirectory& directory,
    ProgramSnapshotExchange& exchange,
    SampleSource* source)
    : residency_(residency), directory_(directory), exchange_(exchange), source_(source) {
    bank_generation_ = directory_.bankGeneration();
}

bool ProgramPrepareController::setBankGeneration(std::uint32_t bank_generation) {
    if (!directory_.setBankGeneration(bank_generation)) {
        setError(ProgramPrepareError::TransitionBusy);
        return false;
    }
    bank_generation_ = bank_generation;
    setError(ProgramPrepareError::None);
    return true;
}

bool ProgramPrepareController::dependencyListIsValid(
    const SampleId* dependencies,
    std::size_t dependency_count) const {
    if (dependencies == nullptr || dependency_count == 0 ||
        dependency_count > ProgramSnapshot::kMaxDependencies) {
        return false;
    }

    std::array<bool, SampleResidencyManager::kMaxResidentSamples> seen{};
    for (std::size_t i = 0; i < dependency_count; ++i) {
        const SampleId sample_id = dependencies[i];
        if (static_cast<std::size_t>(sample_id) >= seen.size() || seen[sample_id]) {
            return false;
        }
        seen[sample_id] = true;
    }
    return true;
}

bool ProgramPrepareController::evictOneSafeCacheSample() {
    SampleId victim = static_cast<SampleId>(SampleResidencyManager::kMaxResidentSamples);
    std::uint64_t oldest_touch = std::numeric_limits<std::uint64_t>::max();
    std::size_t victim_bytes = 0;

    for (std::size_t index = 0; index < SampleResidencyManager::kMaxResidentSamples; ++index) {
        const SampleId sample_id = static_cast<SampleId>(index);
        const ResidentSampleView view = residency_.residentView(sample_id);
        if (!view.valid() || view.pin_count != 0 || view.phase_a_voice_ref_count != 0) {
            continue;
        }
        if (directory_.published(sample_id) != nullptr) {
            continue;
        }

        // records_[sample_id] may be a never-published zero-initialized record
        // or a retired publication record. canReclaim() safely accepts either;
        // a live release-tail Voice Ref makes it return false.
        if (!directory_.canReclaim(&records_[sample_id])) {
            continue;
        }

        if (view.touch_sequence < oldest_touch) {
            oldest_touch = view.touch_sequence;
            victim = sample_id;
            victim_bytes = view.allocated_bytes;
        }
    }

    if (static_cast<std::size_t>(victim) >= SampleResidencyManager::kMaxResidentSamples) {
        return false;
    }
    if (!residency_.evict(victim)) {
        return false;
    }

    ++stats_.cache_evictions;
    stats_.cache_evicted_bytes += victim_bytes;
    return true;
}

ResidentSampleView ProgramPrepareController::ensureResident(SampleId sample_id) {
    ResidentSampleView view = residency_.residentView(sample_id);
    if (view.valid()) {
        ++stats_.samples_reused_resident;
        return view;
    }

    if (source_ == nullptr) {
        setError(ProgramPrepareError::NotResident);
        return ResidentSampleView{};
    }

    SampleDescriptor descriptor{};
    if (!source_->descriptorFor(sample_id, &descriptor) ||
        descriptor.id != sample_id || descriptor.pcm_bytes == 0 ||
        (descriptor.pcm_bytes & 1u) != 0u) {
        ++stats_.source_failures;
        setError(ProgramPrepareError::SourceDescriptorFailed);
        return ResidentSampleView{};
    }

    const ResidencyStats current_stats = residency_.stats();
    if (SampleResidencyManager::alignedBytes(descriptor.pcm_bytes) > current_stats.capacity_bytes) {
        ++stats_.capacity_failures;
        setError(ProgramPrepareError::ResidencyCapacityFailed);
        return ResidentSampleView{};
    }

    SourceFillContext context{source_, sample_id};
    for (std::size_t attempt = 0;
         attempt <= SampleResidencyManager::kMaxResidentSamples;
         ++attempt) {
        if (residency_.loadFromSource(descriptor, fillFromSampleSource, &context)) {
            view = residency_.residentView(sample_id);
            if (!view.valid() || view.descriptor != descriptor) {
                ++stats_.source_failures;
                setError(ProgramPrepareError::SourceLoadFailed);
                return ResidentSampleView{};
            }

            ++stats_.samples_loaded;
            stats_.sample_bytes_loaded += descriptor.pcm_bytes;
            return view;
        }

        const ResidencyError residency_error = residency_.lastError();
        if (!isCapacityError(residency_error)) {
            if (residency_error == ResidencyError::SourceFillFailed) {
                ++stats_.source_failures;
                setError(ProgramPrepareError::SourceLoadFailed);
            } else if (residency_error == ResidencyError::DescriptorMismatch) {
                setError(ProgramPrepareError::ResidencyConflict);
            } else {
                setError(ProgramPrepareError::SourceLoadFailed);
            }
            return ResidentSampleView{};
        }

        if (!evictOneSafeCacheSample()) {
            ++stats_.capacity_failures;
            setError(ProgramPrepareError::ResidencyCapacityFailed);
            return ResidentSampleView{};
        }
        ++stats_.capacity_retries;
    }

    ++stats_.capacity_failures;
    setError(ProgramPrepareError::ResidencyCapacityFailed);
    return ResidentSampleView{};
}

ProgramSnapshot* ProgramPrepareController::selectFreeSnapshot(std::uint8_t channel) {
    if (channel >= snapshots_.size()) {
        return nullptr;
    }

    const ProgramSnapshot* active = exchange_.active(channel);
    const ProgramSnapshot* ready = exchange_.ready(channel);
    ChannelSnapshots& pair = snapshots_[channel];

    if (&pair.a != active && &pair.a != ready) {
        return &pair.a;
    }
    if (&pair.b != active && &pair.b != ready) {
        return &pair.b;
    }
    return nullptr;
}

bool ProgramPrepareController::recordMatchesView(
    const ResidentRecord& record,
    const ResidentSampleView& view) const {
    return record.bank_generation == bank_generation_ &&
           record.sample_generation == view.generation &&
           record.pcm == view.pcm &&
           record.descriptor == view.descriptor;
}

void ProgramPrepareController::initializeRecord(
    ResidentRecord& record,
    const ResidentSampleView& view) {
    record.bank_generation = bank_generation_;
    record.sample_generation = view.generation;
    record.pcm = view.pcm;
    record.descriptor = view.descriptor;
    record.voice_refs.store(0, std::memory_order_release);
}

bool ProgramPrepareController::ensurePublished(
    const ResidentSampleView& view,
    bool* newly_published) {
    if (!view.valid() || newly_published == nullptr ||
        static_cast<std::size_t>(view.sample_id) >= records_.size()) {
        setError(ProgramPrepareError::InvalidArgument);
        return false;
    }

    *newly_published = false;
    ResidentRecord& record = records_[view.sample_id];
    const ResidentRecord* existing = directory_.published(view.sample_id);

    if (existing != nullptr) {
        if (existing != &record || !recordMatchesView(record, view)) {
            setError(ProgramPrepareError::PublicationConflict);
            return false;
        }
        ++stats_.samples_reused_published;
        return true;
    }

    if (record.sample_generation != 0) {
        if (!recordMatchesView(record, view)) {
            if (!directory_.canReclaim(&record)) {
                setError(ProgramPrepareError::RetiringGenerationBusy);
                return false;
            }
            initializeRecord(record, view);
        }
        // Same immutable record may be republished while old release-tail refs
        // still exist. No storage is being reclaimed or reused in that case.
    } else {
        initializeRecord(record, view);
    }

    if (!directory_.publish(view.sample_id, &record)) {
        setError(ProgramPrepareError::PublicationConflict);
        return false;
    }

    *newly_published = true;
    ++stats_.samples_published;
    return true;
}

void ProgramPrepareController::rollbackPinsAndPublications(
    const std::array<SampleId, ProgramSnapshot::kMaxDependencies>& ids,
    const std::array<bool, ProgramSnapshot::kMaxDependencies>& newly_published,
    std::size_t count) {
    for (std::size_t i = count; i > 0; --i) {
        (void)residency_.unpin(ids[i - 1]);
    }

    for (std::size_t i = 0; i < count; ++i) {
        if (!newly_published[i]) {
            continue;
        }
        const ResidentSampleView view = residency_.residentView(ids[i]);
        if (view.valid() && view.pin_count == 0 &&
            directory_.published(ids[i]) == &records_[ids[i]]) {
            (void)directory_.unpublish(ids[i]);
        }
    }
}

bool ProgramPrepareController::prepareImpl(
    bool initial_activation,
    std::uint8_t channel,
    std::uint16_t bank,
    std::uint8_t program,
    std::uint32_t token,
    const SampleId* dependencies,
    std::size_t dependency_count) {
    ++stats_.prepare_attempts;

    if (channel >= ProgramSnapshotExchange::kMidiChannels || token == 0 ||
        bank_generation_ == 0 || !dependencyListIsValid(dependencies, dependency_count)) {
        ++stats_.prepare_failures;
        setError(ProgramPrepareError::InvalidArgument);
        return false;
    }

    if (!initial_activation && exchange_.transitionInFlight(channel)) {
        ++stats_.prepare_failures;
        setError(ProgramPrepareError::TransitionBusy);
        return false;
    }

    ProgramSnapshot* target = selectFreeSnapshot(channel);
    if (target == nullptr) {
        ++stats_.prepare_failures;
        setError(ProgramPrepareError::SnapshotUnavailable);
        return false;
    }

    std::array<SampleId, ProgramSnapshot::kMaxDependencies> pinned_ids{};
    std::array<bool, ProgramSnapshot::kMaxDependencies> newly_published{};
    std::size_t prepared_count = 0;

    for (std::size_t i = 0; i < dependency_count; ++i) {
        const SampleId sample_id = dependencies[i];
        const ResidentSampleView before_pin = ensureResident(sample_id);
        if (!before_pin.valid()) {
            rollbackPinsAndPublications(pinned_ids, newly_published, prepared_count);
            ++stats_.prepare_failures;
            return false;
        }

        if (!residency_.pin(sample_id)) {
            rollbackPinsAndPublications(pinned_ids, newly_published, prepared_count);
            ++stats_.prepare_failures;
            setError(ProgramPrepareError::PinFailed);
            return false;
        }

        pinned_ids[prepared_count] = sample_id;
        ++stats_.samples_pinned;

        const ResidentSampleView pinned_view = residency_.residentView(sample_id);
        bool did_publish = false;
        if (!ensurePublished(pinned_view, &did_publish)) {
            ++prepared_count;
            newly_published[prepared_count - 1] = did_publish;
            rollbackPinsAndPublications(pinned_ids, newly_published, prepared_count);
            ++stats_.prepare_failures;
            return false;
        }

        newly_published[prepared_count] = did_publish;
        ++prepared_count;
    }

    *target = ProgramSnapshot{};
    target->channel = channel;
    target->bank = bank;
    target->program = program;
    target->token = token;
    target->bank_generation = bank_generation_;
    target->dependency_count = static_cast<std::uint16_t>(dependency_count);
    for (std::size_t i = 0; i < dependency_count; ++i) {
        target->sample_ids[i] = dependencies[i];
    }

    const bool published_snapshot = initial_activation
        ? exchange_.setInitialActive(target)
        : exchange_.publishReady(target);
    if (!published_snapshot) {
        rollbackPinsAndPublications(pinned_ids, newly_published, prepared_count);
        ++stats_.prepare_failures;
        setError(initial_activation
            ? ProgramPrepareError::InitialActivationFailed
            : ProgramPrepareError::ReadyPublishFailed);
        return false;
    }

    ++stats_.prepare_successes;
    setError(ProgramPrepareError::None);
    return true;
}

bool ProgramPrepareController::activateInitialProgram(
    std::uint8_t channel,
    std::uint16_t bank,
    std::uint8_t program,
    std::uint32_t token,
    const SampleId* dependencies,
    std::size_t dependency_count) {
    return prepareImpl(
        true,
        channel,
        bank,
        program,
        token,
        dependencies,
        dependency_count);
}

bool ProgramPrepareController::prepareProgram(
    std::uint8_t channel,
    std::uint16_t bank,
    std::uint8_t program,
    std::uint32_t token,
    const SampleId* dependencies,
    std::size_t dependency_count) {
    return prepareImpl(
        false,
        channel,
        bank,
        program,
        token,
        dependencies,
        dependency_count);
}

bool ProgramPrepareController::retireSnapshot(const ProgramSnapshot& snapshot) {
    bool success = true;

    for (std::size_t i = 0; i < snapshot.dependency_count; ++i) {
        if (residency_.unpin(snapshot.sample_ids[i])) {
            ++stats_.retired_pins;
        } else {
            success = false;
        }
    }

    // After all old pins are removed, unpublish only samples with no remaining
    // prepared/active snapshot owner. Existing release-tail Voice Refs remain
    // valid on the retiring ResidentRecord and defer reclamation automatically.
    for (std::size_t i = 0; i < snapshot.dependency_count; ++i) {
        const SampleId sample_id = snapshot.sample_ids[i];
        const ResidentSampleView view = residency_.residentView(sample_id);
        if (!view.valid() || view.pin_count != 0) {
            continue;
        }
        if (directory_.published(sample_id) == &records_[sample_id]) {
            if (directory_.unpublish(sample_id) != nullptr) {
                ++stats_.samples_unpublished_after_retire;
            }
        }
    }

    return success;
}

ProgramCommitAck ProgramPrepareController::acknowledgeAndRetire(
    std::uint8_t channel,
    std::uint32_t token) {
    ProgramCommitAck ack = exchange_.acknowledgeCommit(channel, token);
    if (!ack.valid()) {
        return ack;
    }

    ++stats_.retire_acks;
    if (ack.retired != nullptr && !retireSnapshot(*ack.retired)) {
        setError(ProgramPrepareError::RetireFailed);
        return ack;
    }

    setError(ProgramPrepareError::None);
    return ack;
}

}  // namespace gm_synth::sample
