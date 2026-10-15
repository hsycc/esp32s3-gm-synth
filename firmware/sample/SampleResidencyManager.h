#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace gm_synth::sample {

using SampleId = std::uint32_t;

struct SampleDescriptor {
    SampleId id = 0;
    std::uint32_t pcm_bytes = 0;
    std::uint32_t frames = 0;
    std::uint32_t loop_start = 0;
    std::uint32_t loop_end = 0;
    std::uint32_t sample_rate = 0;
    std::uint8_t root_key = 60;
    std::int8_t pitch_correction = 0;
    std::uint16_t flags = 0;

    bool operator==(const SampleDescriptor& other) const;
    bool operator!=(const SampleDescriptor& other) const { return !(*this == other); }
};

struct SampleRef {
    SampleId sample_id = 0;
    std::uint32_t generation = 0;
    std::uint32_t lease_token = 0;
    const std::int16_t* pcm = nullptr;
    SampleDescriptor descriptor{};

    bool valid() const { return pcm != nullptr && lease_token != 0; }
};

// Control/storage-domain read-only view of one resident arena entry. This is
// NOT an audio-side ownership token and does not increment a Voice Ref. The
// view is valid only while control keeps the entry pinned/not-evicted.
struct ResidentSampleView {
    SampleId sample_id = 0;
    std::uint32_t generation = 0;
    const std::int16_t* pcm = nullptr;
    SampleDescriptor descriptor{};
    std::uint16_t pin_count = 0;
    std::uint16_t phase_a_voice_ref_count = 0;

    bool valid() const { return pcm != nullptr && generation != 0; }
};

enum class ResidencyError : std::uint8_t {
    None = 0,
    NotInitialized,
    InvalidArgument,
    DescriptorMismatch,
    MetadataFull,
    NoContiguousSpace,
    SourceFillFailed,
    NotResident,
    Busy,
    LeaseExhausted,
    StaleReference,
    PinUnderflow,
};

struct ResidencyStats {
    std::size_t capacity_bytes = 0;
    std::size_t used_bytes = 0;
    std::size_t high_water_bytes = 0;
    std::size_t largest_free_span_bytes = 0;
    std::size_t free_span_count = 0;
    std::size_t resident_sample_count = 0;
    std::size_t pinned_sample_count = 0;
    std::size_t referenced_sample_count = 0;
    std::size_t active_lease_count = 0;
    std::uint32_t allocation_failures = 0;
    std::uint32_t stale_reference_faults = 0;
    std::uint32_t descriptor_conflicts = 0;
};

// Called only from the control/storage domain. destination points directly into
// the reserved arena allocation (PSRAM on target). The callback may perform
// chunked storage reads into destination and must return true only after exactly
// pcm_bytes bytes are valid.
using SampleFillCallback = bool (*)(
    void* context,
    std::uint8_t* destination,
    std::size_t pcm_bytes);

// Phase-A/M3 residency core.
//
// The class deliberately owns no heap allocations. The caller supplies the
// PCM arena (PSRAM on ESP32-S3, an aligned byte buffer in host tests). Metadata
// capacity is fixed so load/acquire/release behavior remains bounded.
//
// Threading note: allocator/hash/pin/load methods and residentView() belong to
// the single control/storage domain. Audio cross-core access is provided
// separately by PublishedSampleDirectory in M3 Phase B.
class SampleResidencyManager {
public:
    static constexpr std::size_t kAlignment = 4;
    static constexpr std::size_t kMaxResidentSamples = 512;
    static constexpr std::size_t kHashSlots = 1024;
    static constexpr std::size_t kMaxFreeSpans = kMaxResidentSamples + 1;
    static constexpr std::size_t kMaxVoiceLeases = 64;

    SampleResidencyManager();

    bool begin(void* arena, std::size_t arena_bytes);

    // Control/storage-domain convenience path for already-buffered PCM.
    // Re-loading an identical resident SampleId reuses the existing allocation.
    bool load(const SampleDescriptor& descriptor, const void* pcm, std::size_t pcm_bytes);

    // Control/storage-domain direct-fill transaction:
    //   reserve free arena span -> fill callback writes directly into arena ->
    //   commit hash/Entry only after successful fill.
    //
    // No same-sized staging buffer is required. If fill returns false, the
    // reserved span is returned and the SampleId remains non-resident.
    // Existing identical samples are reused without invoking fill.
    //
    // The callback executes synchronously on the caller's control/storage task;
    // no other mutating SampleResidencyManager operation may run concurrently.
    bool loadFromSource(
        const SampleDescriptor& descriptor,
        SampleFillCallback fill,
        void* context);

    bool isResident(SampleId sample_id) const;

    // Control-only observation. No ownership is acquired; callers must pin the
    // entry before publishing a returned pointer to another domain.
    ResidentSampleView residentView(SampleId sample_id) const;

    // Program/channel residency ownership. A pinned sample is not evictable.
    bool pin(SampleId sample_id);
    bool unpin(SampleId sample_id);

    // Phase-A Voice ownership used by host lifetime tests. Final cross-core
    // audio ownership is handled by PublishedSampleDirectory.
    SampleRef acquire(SampleId sample_id);
    bool release(const SampleRef& ref);

    // Eviction succeeds only when pinCount == 0 && phase-A voiceRefCount == 0.
    // Final runtime eviction must additionally ensure any published record has
    // been unpublished and is reclaimable.
    bool evict(SampleId sample_id);

    // Safe rebuild primitive for tests/control code. It refuses to invalidate
    // either a program pin or a live phase-A voice reference.
    bool reset();

    ResidencyStats stats() const;
    ResidencyError lastError() const { return last_error_; }

    static std::size_t alignedBytes(std::size_t bytes);

private:
    struct Entry {
        bool occupied = false;
        SampleDescriptor descriptor{};
        std::size_t offset = 0;
        std::size_t allocated_bytes = 0;
        std::uint16_t pin_count = 0;
        std::uint16_t voice_ref_count = 0;
        std::uint32_t generation = 0;
        std::uint64_t touch_sequence = 0;
    };

    enum class HashState : std::uint8_t { Empty = 0, Occupied, Tombstone };

    struct HashSlot {
        HashState state = HashState::Empty;
        SampleId sample_id = 0;
        std::uint16_t entry_index = 0;
    };

    struct Span {
        std::size_t offset = 0;
        std::size_t bytes = 0;
    };

    struct Lease {
        bool active = false;
        std::uint32_t token = 0;
        SampleId sample_id = 0;
        std::uint32_t generation = 0;
        std::uint16_t entry_index = 0;
    };

    static_assert((kHashSlots & (kHashSlots - 1)) == 0, "kHashSlots must be a power of two");

    std::uint8_t* arena_ = nullptr;
    std::size_t arena_bytes_ = 0;
    bool initialized_ = false;

    std::array<Entry, kMaxResidentSamples> entries_{};
    std::array<HashSlot, kHashSlots> hash_{};
    std::array<Span, kMaxFreeSpans> free_spans_{};
    std::size_t free_span_count_ = 0;
    std::array<Lease, kMaxVoiceLeases> leases_{};

    std::uint32_t next_generation_ = 1;
    std::uint32_t next_lease_token_ = 1;
    std::uint64_t touch_sequence_ = 1;
    std::size_t used_bytes_ = 0;
    std::size_t high_water_bytes_ = 0;
    std::uint32_t allocation_failures_ = 0;
    std::uint32_t stale_reference_faults_ = 0;
    std::uint32_t descriptor_conflicts_ = 0;
    ResidencyError last_error_ = ResidencyError::None;

    static std::size_t hashIndex(SampleId sample_id);
    int findEntryIndex(SampleId sample_id) const;
    int findFreeEntryIndex() const;
    bool insertHash(SampleId sample_id, std::uint16_t entry_index);
    void removeHash(SampleId sample_id);

    bool allocateSpan(std::size_t bytes, std::size_t* offset);
    bool freeSpan(std::size_t offset, std::size_t bytes);
    void removeFreeSpan(std::size_t index);
    void coalesceFreeSpans();

    int allocateLease();
    std::uint32_t nextGeneration();
    std::uint32_t nextLeaseToken();
    std::uint64_t nextTouchSequence();

    void clearState();
    void setError(ResidencyError error) { last_error_ = error; }
};

}  // namespace gm_synth::sample
