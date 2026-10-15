#pragma once

#include "SampleResidencyManager.h"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>

namespace gm_synth::sample {

// Immutable after publication except for voice_refs, which is the deferred-
// reclamation reference count. The owner of a ResidentRecord must keep the
// record object and its PCM allocation alive until canReclaim(record) is true.
struct ResidentRecord {
    std::uint32_t bank_generation = 0;
    std::uint32_t sample_generation = 0;
    const std::int16_t* pcm = nullptr;
    SampleDescriptor descriptor{};
    std::atomic<std::uint32_t> voice_refs{0};
};

struct PublishedVoiceRef {
    const ResidentRecord* record = nullptr;
    SampleId sample_id = 0;
    std::uint32_t bank_generation = 0;
    std::uint32_t sample_generation = 0;

    bool valid() const { return record != nullptr; }
};

struct PublicationStats {
    std::uint32_t bank_generation = 0;
    std::size_t published_sample_count = 0;
    std::uint32_t active_acquirers = 0;
    std::uint64_t acquire_successes = 0;
    std::uint64_t acquire_misses = 0;
    std::uint64_t acquire_retries = 0;
    std::uint64_t stale_release_faults = 0;
    std::uint64_t publish_conflicts = 0;
};

// Audio-facing publication surface.
//
// The control/storage domain owns ResidentRecord objects and PSRAM allocation.
// Audio only touches this directory, immutable ResidentRecord fields and PCM.
//
// Important reclamation rule:
//   unpublish(record) -> wait until canReclaim(record) -> free/reuse record+PCM
//
// active_acquirers_ acts as a small RCU-style grace-period guard. This is
// required because a pointer load followed by voice_refs++ alone is not enough
// to prevent control from reclaiming the record between those two operations.
class PublishedSampleDirectory {
public:
    static constexpr std::size_t kMaxSamples = SampleResidencyManager::kMaxResidentSamples;
    static constexpr std::size_t kAcquireAttempts = 2;

    PublishedSampleDirectory();

    // May only change when nothing is published and no Voice Ref / acquire
    // critical section from the previous bank remains.
    bool setBankGeneration(std::uint32_t bank_generation);
    std::uint32_t bankGeneration() const {
        return bank_generation_.load(std::memory_order_acquire);
    }

    // Release-publishes a fully initialized record. Re-publishing the same
    // pointer is idempotent; replacing a live pointer is rejected and requires
    // explicit unpublish first.
    bool publish(SampleId sample_id, const ResidentRecord* record);

    // Removes the record from new acquisitions. The returned pointer enters
    // retiring state conceptually; it is NOT yet safe to free.
    const ResidentRecord* unpublish(SampleId sample_id);

    // Bounded audio-side acquire. No allocation, storage I/O, mutex or scan.
    PublishedVoiceRef acquire(SampleId sample_id);
    bool release(const PublishedVoiceRef& ref);

    // Control-side deferred-reclamation predicate. It verifies the record is
    // no longer published, no acquire critical section is in flight, and the
    // exact record has no live Voice Refs.
    bool canReclaim(const ResidentRecord* record) const;

    const ResidentRecord* published(SampleId sample_id) const;
    PublicationStats stats() const;

private:
    struct Slot {
        Slot() noexcept : published(nullptr) {}
        std::atomic<const ResidentRecord*> published;
    };

    bool validSampleId(SampleId sample_id) const {
        return static_cast<std::size_t>(sample_id) < slots_.size();
    }
    bool anyPublished() const;
    bool isRecordPublished(const ResidentRecord* record) const;

    std::array<Slot, kMaxSamples> slots_{};
    std::atomic<std::uint32_t> bank_generation_{1};
    std::atomic<std::uint32_t> active_acquirers_{0};

    std::atomic<std::uint64_t> acquire_successes_{0};
    std::atomic<std::uint64_t> acquire_misses_{0};
    std::atomic<std::uint64_t> acquire_retries_{0};
    std::atomic<std::uint64_t> stale_release_faults_{0};
    std::atomic<std::uint64_t> publish_conflicts_{0};
};

}  // namespace gm_synth::sample
