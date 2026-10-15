#include "PublishedSampleDirectory.h"

#include <limits>

namespace gm_synth::sample {

PublishedSampleDirectory::PublishedSampleDirectory() = default;

bool PublishedSampleDirectory::anyPublished() const {
    for (const Slot& slot : slots_) {
        if (slot.published.load(std::memory_order_acquire) != nullptr) {
            return true;
        }
    }
    return false;
}

bool PublishedSampleDirectory::isRecordPublished(const ResidentRecord* record) const {
    if (record == nullptr) {
        return false;
    }
    for (const Slot& slot : slots_) {
        if (slot.published.load(std::memory_order_acquire) == record) {
            return true;
        }
    }
    return false;
}

bool PublishedSampleDirectory::setBankGeneration(std::uint32_t bank_generation) {
    if (bank_generation == 0 || anyPublished() ||
        active_acquirers_.load(std::memory_order_acquire) != 0) {
        return false;
    }

    // Changing bank identity while an unpublished old-bank record still has a
    // Voice Ref would make reclamation/accounting ambiguous. The caller owns
    // those records, so enforce the stronger contract through publication use:
    // bank generation changes only after all old records are reclaimable.
    bank_generation_.store(bank_generation, std::memory_order_release);
    return true;
}

bool PublishedSampleDirectory::publish(SampleId sample_id, const ResidentRecord* record) {
    if (!validSampleId(sample_id) || record == nullptr || record->pcm == nullptr ||
        record->descriptor.id != sample_id || record->descriptor.pcm_bytes == 0 ||
        record->bank_generation != bank_generation_.load(std::memory_order_acquire) ||
        record->sample_generation == 0) {
        return false;
    }

    Slot& slot = slots_[static_cast<std::size_t>(sample_id)];
    const ResidentRecord* expected = nullptr;
    if (slot.published.compare_exchange_strong(
            expected,
            record,
            std::memory_order_release,
            std::memory_order_acquire)) {
        return true;
    }

    if (expected == record) {
        return true;
    }

    publish_conflicts_.fetch_add(1, std::memory_order_relaxed);
    return false;
}

const ResidentRecord* PublishedSampleDirectory::unpublish(SampleId sample_id) {
    if (!validSampleId(sample_id)) {
        return nullptr;
    }
    return slots_[static_cast<std::size_t>(sample_id)].published.exchange(
        nullptr,
        std::memory_order_acq_rel);
}

PublishedVoiceRef PublishedSampleDirectory::acquire(SampleId sample_id) {
    PublishedVoiceRef result{};
    if (!validSampleId(sample_id)) {
        acquire_misses_.fetch_add(1, std::memory_order_relaxed);
        return result;
    }

    active_acquirers_.fetch_add(1, std::memory_order_acq_rel);
    Slot& slot = slots_[static_cast<std::size_t>(sample_id)];

    for (std::size_t attempt = 0; attempt < kAcquireAttempts; ++attempt) {
        const ResidentRecord* record = slot.published.load(std::memory_order_acquire);
        if (record == nullptr) {
            break;
        }

        const std::uint32_t bank_generation = bank_generation_.load(std::memory_order_acquire);
        if (record->descriptor.id != sample_id || record->bank_generation != bank_generation) {
            break;
        }

        record->voice_refs.fetch_add(1, std::memory_order_acq_rel);

        const ResidentRecord* confirm = slot.published.load(std::memory_order_acquire);
        const std::uint32_t confirm_bank = bank_generation_.load(std::memory_order_acquire);
        if (confirm == record && record->bank_generation == confirm_bank) {
            result.record = record;
            result.sample_id = sample_id;
            result.bank_generation = record->bank_generation;
            result.sample_generation = record->sample_generation;
            acquire_successes_.fetch_add(1, std::memory_order_relaxed);
            active_acquirers_.fetch_sub(1, std::memory_order_release);
            return result;
        }

        record->voice_refs.fetch_sub(1, std::memory_order_acq_rel);
        acquire_retries_.fetch_add(1, std::memory_order_relaxed);
    }

    acquire_misses_.fetch_add(1, std::memory_order_relaxed);
    active_acquirers_.fetch_sub(1, std::memory_order_release);
    return result;
}

bool PublishedSampleDirectory::release(const PublishedVoiceRef& ref) {
    if (!ref.valid()) {
        stale_release_faults_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }

    const ResidentRecord* record = ref.record;
    if (record->descriptor.id != ref.sample_id ||
        record->bank_generation != ref.bank_generation ||
        record->sample_generation != ref.sample_generation) {
        stale_release_faults_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }

    std::uint32_t count = record->voice_refs.load(std::memory_order_acquire);
    while (count != 0) {
        if (record->voice_refs.compare_exchange_weak(
                count,
                count - 1,
                std::memory_order_acq_rel,
                std::memory_order_acquire)) {
            return true;
        }
    }

    stale_release_faults_.fetch_add(1, std::memory_order_relaxed);
    return false;
}

bool PublishedSampleDirectory::canReclaim(const ResidentRecord* record) const {
    if (record == nullptr || isRecordPublished(record)) {
        return false;
    }
    if (active_acquirers_.load(std::memory_order_acquire) != 0) {
        return false;
    }
    return record->voice_refs.load(std::memory_order_acquire) == 0;
}

const ResidentRecord* PublishedSampleDirectory::published(SampleId sample_id) const {
    if (!validSampleId(sample_id)) {
        return nullptr;
    }
    return slots_[static_cast<std::size_t>(sample_id)].published.load(std::memory_order_acquire);
}

PublicationStats PublishedSampleDirectory::stats() const {
    PublicationStats out{};
    out.bank_generation = bank_generation_.load(std::memory_order_acquire);
    for (const Slot& slot : slots_) {
        if (slot.published.load(std::memory_order_acquire) != nullptr) {
            ++out.published_sample_count;
        }
    }
    out.active_acquirers = active_acquirers_.load(std::memory_order_acquire);
    out.acquire_successes = acquire_successes_.load(std::memory_order_relaxed);
    out.acquire_misses = acquire_misses_.load(std::memory_order_relaxed);
    out.acquire_retries = acquire_retries_.load(std::memory_order_relaxed);
    out.stale_release_faults = stale_release_faults_.load(std::memory_order_relaxed);
    out.publish_conflicts = publish_conflicts_.load(std::memory_order_relaxed);
    return out;
}

}  // namespace gm_synth::sample
