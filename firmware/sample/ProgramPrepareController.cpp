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

    // Prepare is a control-domain operation and dependency lists are bounded at
    // 512 entries. SampleId itself is bank-local and may be much larger than
    // the simultaneous resident count. An O(n^2) duplicate check avoids extra
    // automatic scratch on the small ESP-IDF control/main task stack.
    for (std::size_t i = 0; i < dependency_count; ++i) {
        for (std::size_t previous = 0; previous < i; ++previous) {
            if (dependencies[previous] == dependencies[i]) {
                return false;
            }
        }
    }
    return true;
}

ResidentRecord* ProgramPrepareController::findRecord(SampleId sample_id) {
    for (ResidentRecord& record : records_) {
        if (record.sample_generation != 0 && record.descriptor.id == sample_id) {
            return &record;
        }
    }
    return nullptr;
}

ResidentRecord* ProgramPrepareController::selectReusableRecord() {
    for (ResidentRecord& record : records_) {
        if (record.sample_generation == 0) {
            return &record;
        }
    }
    for (ResidentRecord& record : records_) {
        if (directory_.canReclaim(&record)) {
            return &record;
        }
    }
    return nullptr;
}

bool ProgramPrepareController::evictOneSafeCacheSample() {
    SampleId victim = 0;
    bool found_victim = false;
    std::uint64_t oldest_touch = std::numeric_limits<std::uint64_t>::max();
    std::size_t victim_bytes = 0;

    for (std::size_t index = 0; index < SampleResidencyManager::kMaxResidentSamples; ++index) {
        const ResidentSampleView view = residency_.residentViewAt(index);
        if (!view.valid() || view.pin_count != 0 || view.phase_a_voice_ref_count != 0) {
            continue;
        }

        const SampleId sample_id = view.sample_id;
        if (directory_.published(sample_id) != nullptr) {
            continue;
        }

        // A direct-to-residency cache entry may never have been published and
        // therefore may have no ResidentRecord. If a record exists, release-tail
        // ownership must be fully reclaimed before its PCM can be evicted.
        ResidentRecord* record = findRecord(sample_id);
        if (record != nullptr && !directory_.canReclaim(record)) {
            continue;
        }

        if (view.touch_sequence < oldest_touch) {
            oldest_touch = view.touch_sequence;
            victim = sample_id;
            victim_bytes = view.allocated_bytes;
            found_victim = true;
        }
    }

    if (!found_victim || !residency_.evict(victim)) {
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
    if (!view.valid() || newly_published == nullptr) {
        setError(ProgramPrepareError::InvalidArgument);
        return false;
    }

    *newly_published = false;
    ResidentRecord* record = findRecord(view.sample_id);
    const ResidentRecord* existing = directory_.published(view.sample_id);

    if (existing != nullptr) {
        if (record == nullptr || existing != record || !recordMatchesView(*record, view)) {
            setError(ProgramPrepareError::PublicationConflict);
            return false;
        }
        ++stats_.samples_reused_published;
        return true;
    }

    if (record == nullptr) {
        record = selectReusableRecord();
        if (record == nullptr) {
            setError(ProgramPrepareError::RetiringGenerationBusy);
            return false;
        }
        initializeRecord(*record, view);
    } else if (!recordMatchesView(*record, view)) {
        if (!directory_.canReclaim(record)) {
            setError(ProgramPrepareError::RetiringGenerationBusy);
            return false;
        }
        initializeRecord(*record, view);
    }

    // The same immutable record may be republished while an old release-tail
    // owns it only when it still matches the exact resident generation. A
    // different generation is never installed until canReclaim(record) is true.
    if (!directory_.publish(view.sample_id, record)) {
        setError(ProgramPrepareError::PublicationConflict);
        return false;
    }

    *newly_published = true;
    ++stats_.samples_published;
    return true;
}

void ProgramPrepareController::rollbackPinsAndPublications(
    const ProgramSnapshot& working_snapshot,
    const PublicationBitmap& newly_published,
    std::size_t count) {
    for (std::size_t i = count; i > 0; --i) {
        (void)residency_.unpin(working_snapshot.sample_ids[i - 1]);
    }

    for (std::size_t i = 0; i < count; ++i) {
        const std::size_t word = i / 64u;
        const std::size_t bit = i % 64u;
        const bool was_newly_published =
            (newly_published[word] & (std::uint64_t{1} << bit)) != 0;
        if (!was_newly_published) {
            continue;
        }

        const SampleId sample_id = working_snapshot.sample_ids[i];
        const ResidentSampleView view = residency_.residentView(sample_id);
        ResidentRecord* record = findRecord(sample_id);
        if (view.valid() && view.pin_count == 0 && record != nullptr &&
            directory_.published(sample_id) == record) {
            (void)directory_.unpublish(sample_id);
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

    // Reuse the selected, unpublished snapshot's existing dependency array as
    // rollback storage. The only automatic transaction scratch is a 512-bit
    // bitmap (64 bytes), keeping Prepare safe on the small ESP-IDF task stack.
    target->token = 0;
    target->dependency_count = 0;
    PublicationBitmap newly_published{};
    std::size_t prepared_count = 0;

    for (std::size_t i = 0; i < dependency_count; ++i) {
        const SampleId sample_id = dependencies[i];
        const ResidentSampleView before_pin = ensureResident(sample_id);
        if (!before_pin.valid()) {
            rollbackPinsAndPublications(*target, newly_published, prepared_count);
            ++stats_.prepare_failures;
            return false;
        }

        if (!residency_.pin(sample_id)) {
            rollbackPinsAndPublications(*target, newly_published, prepared_count);
            ++stats_.prepare_failures;
            setError(ProgramPrepareError::PinFailed);
            return false;
        }

        target->sample_ids[prepared_count] = sample_id;
        ++stats_.samples_pinned;

        const ResidentSampleView pinned_view = residency_.residentView(sample_id);
        bool did_publish = false;
        if (!ensurePublished(pinned_view, &did_publish)) {
            if (did_publish) {
                const std::size_t word = prepared_count / 64u;
                const std::size_t bit = prepared_count % 64u;
                newly_published[word] |= std::uint64_t{1} << bit;
            }
            ++prepared_count;
            rollbackPinsAndPublications(*target, newly_published, prepared_count);
            ++stats_.prepare_failures;
            return false;
        }

        if (did_publish) {
            const std::size_t word = prepared_count / 64u;
            const std::size_t bit = prepared_count % 64u;
            newly_published[word] |= std::uint64_t{1} << bit;
        }
        ++prepared_count;
    }

    target->channel = channel;
    target->bank = bank;
    target->program = program;
    target->token = token;
    target->bank_generation = bank_generation_;
    target->dependency_count = static_cast<std::uint16_t>(dependency_count);

    const bool published_snapshot = initial_activation
        ? exchange_.setInitialActive(target)
        : exchange_.publishReady(target);
    if (!published_snapshot) {
        target->token = 0;
        target->dependency_count = 0;
        rollbackPinsAndPublications(*target, newly_published, prepared_count);
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
        ResidentRecord* record = findRecord(sample_id);
        if (record != nullptr && directory_.published(sample_id) == record) {
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
