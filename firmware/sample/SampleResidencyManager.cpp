#include "SampleResidencyManager.h"

#include <cstring>
#include <limits>

namespace gm_synth::sample {

bool SampleDescriptor::operator==(const SampleDescriptor& other) const {
    return id == other.id &&
           pcm_bytes == other.pcm_bytes &&
           frames == other.frames &&
           loop_start == other.loop_start &&
           loop_end == other.loop_end &&
           sample_rate == other.sample_rate &&
           root_key == other.root_key &&
           pitch_correction == other.pitch_correction &&
           flags == other.flags;
}

SampleResidencyManager::SampleResidencyManager() = default;

std::size_t SampleResidencyManager::alignedBytes(std::size_t bytes) {
    if (bytes == 0) {
        return 0;
    }
    const std::size_t mask = kAlignment - 1;
    return (bytes + mask) & ~mask;
}

std::size_t SampleResidencyManager::hashIndex(SampleId sample_id) {
    return (static_cast<std::size_t>(sample_id) * 2654435761u) & (kHashSlots - 1);
}

bool SampleResidencyManager::begin(void* arena, std::size_t arena_bytes) {
    if (arena == nullptr || arena_bytes < kAlignment ||
        (reinterpret_cast<std::uintptr_t>(arena) & (kAlignment - 1)) != 0) {
        setError(ResidencyError::InvalidArgument);
        return false;
    }

    arena_ = static_cast<std::uint8_t*>(arena);
    arena_bytes_ = arena_bytes & ~(kAlignment - 1);
    initialized_ = true;

    entries_.fill(Entry{});
    hash_.fill(HashSlot{});
    free_spans_.fill(Span{});
    leases_.fill(Lease{});
    free_span_count_ = 1;
    free_spans_[0] = Span{0, arena_bytes_};

    next_generation_ = 1;
    next_lease_token_ = 1;
    touch_sequence_ = 1;
    used_bytes_ = 0;
    high_water_bytes_ = 0;
    allocation_failures_ = 0;
    stale_reference_faults_ = 0;
    descriptor_conflicts_ = 0;
    setError(ResidencyError::None);
    return true;
}

int SampleResidencyManager::findEntryIndex(SampleId sample_id) const {
    std::size_t index = hashIndex(sample_id);
    for (std::size_t probe = 0; probe < kHashSlots; ++probe) {
        const HashSlot& slot = hash_[index];
        if (slot.state == HashState::Empty) {
            return -1;
        }
        if (slot.state == HashState::Occupied && slot.sample_id == sample_id) {
            const std::size_t entry_index = slot.entry_index;
            if (entry_index < entries_.size() && entries_[entry_index].occupied &&
                entries_[entry_index].descriptor.id == sample_id) {
                return static_cast<int>(entry_index);
            }
            return -1;
        }
        index = (index + 1) & (kHashSlots - 1);
    }
    return -1;
}

int SampleResidencyManager::findFreeEntryIndex() const {
    for (std::size_t i = 0; i < entries_.size(); ++i) {
        if (!entries_[i].occupied) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

bool SampleResidencyManager::insertHash(SampleId sample_id, std::uint16_t entry_index) {
    std::size_t index = hashIndex(sample_id);
    std::size_t first_tombstone = kHashSlots;

    for (std::size_t probe = 0; probe < kHashSlots; ++probe) {
        HashSlot& slot = hash_[index];
        if (slot.state == HashState::Occupied && slot.sample_id == sample_id) {
            slot.entry_index = entry_index;
            return true;
        }
        if (slot.state == HashState::Tombstone && first_tombstone == kHashSlots) {
            first_tombstone = index;
        }
        if (slot.state == HashState::Empty) {
            const std::size_t target = first_tombstone == kHashSlots ? index : first_tombstone;
            hash_[target] = HashSlot{HashState::Occupied, sample_id, entry_index};
            return true;
        }
        index = (index + 1) & (kHashSlots - 1);
    }

    if (first_tombstone != kHashSlots) {
        hash_[first_tombstone] = HashSlot{HashState::Occupied, sample_id, entry_index};
        return true;
    }
    return false;
}

void SampleResidencyManager::removeHash(SampleId sample_id) {
    std::size_t index = hashIndex(sample_id);
    for (std::size_t probe = 0; probe < kHashSlots; ++probe) {
        HashSlot& slot = hash_[index];
        if (slot.state == HashState::Empty) {
            return;
        }
        if (slot.state == HashState::Occupied && slot.sample_id == sample_id) {
            slot.state = HashState::Tombstone;
            return;
        }
        index = (index + 1) & (kHashSlots - 1);
    }
}

void SampleResidencyManager::removeFreeSpan(std::size_t index) {
    for (std::size_t i = index + 1; i < free_span_count_; ++i) {
        free_spans_[i - 1] = free_spans_[i];
    }
    if (free_span_count_ > 0) {
        --free_span_count_;
        free_spans_[free_span_count_] = Span{};
    }
}

bool SampleResidencyManager::allocateSpan(std::size_t bytes, std::size_t* offset) {
    const std::size_t aligned = alignedBytes(bytes);
    if (aligned == 0 || offset == nullptr) {
        return false;
    }

    for (std::size_t i = 0; i < free_span_count_; ++i) {
        Span& span = free_spans_[i];
        if (span.bytes < aligned) {
            continue;
        }
        *offset = span.offset;
        span.offset += aligned;
        span.bytes -= aligned;
        if (span.bytes == 0) {
            removeFreeSpan(i);
        }
        return true;
    }
    return false;
}

void SampleResidencyManager::coalesceFreeSpans() {
    if (free_span_count_ < 2) {
        return;
    }

    std::size_t i = 0;
    while (i + 1 < free_span_count_) {
        Span& current = free_spans_[i];
        Span& next = free_spans_[i + 1];
        if (current.offset + current.bytes == next.offset) {
            current.bytes += next.bytes;
            removeFreeSpan(i + 1);
        } else {
            ++i;
        }
    }
}

bool SampleResidencyManager::freeSpan(std::size_t offset, std::size_t bytes) {
    const std::size_t aligned = alignedBytes(bytes);
    if (aligned == 0 || offset + aligned > arena_bytes_ || free_span_count_ >= kMaxFreeSpans) {
        return false;
    }

    std::size_t position = 0;
    while (position < free_span_count_ && free_spans_[position].offset < offset) {
        ++position;
    }

    for (std::size_t i = free_span_count_; i > position; --i) {
        free_spans_[i] = free_spans_[i - 1];
    }
    free_spans_[position] = Span{offset, aligned};
    ++free_span_count_;
    coalesceFreeSpans();
    return true;
}

std::uint32_t SampleResidencyManager::nextGeneration() {
    std::uint32_t value = next_generation_++;
    if (value == 0) {
        value = next_generation_++;
    }
    if (next_generation_ == 0) {
        ++next_generation_;
    }
    return value;
}

std::uint32_t SampleResidencyManager::nextLeaseToken() {
    std::uint32_t value = next_lease_token_++;
    if (value == 0) {
        value = next_lease_token_++;
    }
    if (next_lease_token_ == 0) {
        ++next_lease_token_;
    }
    return value;
}

std::uint64_t SampleResidencyManager::nextTouchSequence() {
    std::uint64_t value = touch_sequence_++;
    if (value == 0) {
        value = touch_sequence_++;
    }
    if (touch_sequence_ == 0) {
        ++touch_sequence_;
    }
    return value;
}

int SampleResidencyManager::allocateLease() {
    for (std::size_t i = 0; i < leases_.size(); ++i) {
        if (!leases_[i].active) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

bool SampleResidencyManager::load(
    const SampleDescriptor& descriptor,
    const void* pcm,
    std::size_t pcm_bytes) {
    if (!initialized_) {
        setError(ResidencyError::NotInitialized);
        return false;
    }
    if (pcm == nullptr || descriptor.pcm_bytes == 0 ||
        descriptor.pcm_bytes != pcm_bytes || (descriptor.pcm_bytes & 1u) != 0u) {
        setError(ResidencyError::InvalidArgument);
        return false;
    }

    const int existing_index = findEntryIndex(descriptor.id);
    if (existing_index >= 0) {
        Entry& existing = entries_[static_cast<std::size_t>(existing_index)];
        if (existing.descriptor != descriptor) {
            ++descriptor_conflicts_;
            setError(ResidencyError::DescriptorMismatch);
            return false;
        }
        existing.touch_sequence = nextTouchSequence();
        setError(ResidencyError::None);
        return true;
    }

    const int free_entry_index = findFreeEntryIndex();
    if (free_entry_index < 0) {
        ++allocation_failures_;
        setError(ResidencyError::MetadataFull);
        return false;
    }

    const std::size_t allocation_bytes = alignedBytes(descriptor.pcm_bytes);
    std::size_t offset = 0;
    if (!allocateSpan(allocation_bytes, &offset)) {
        ++allocation_failures_;
        setError(ResidencyError::NoContiguousSpace);
        return false;
    }

    const std::uint16_t entry_index = static_cast<std::uint16_t>(free_entry_index);
    if (!insertHash(descriptor.id, entry_index)) {
        (void)freeSpan(offset, allocation_bytes);
        ++allocation_failures_;
        setError(ResidencyError::MetadataFull);
        return false;
    }

    std::memcpy(arena_ + offset, pcm, descriptor.pcm_bytes);
    if (allocation_bytes > descriptor.pcm_bytes) {
        std::memset(
            arena_ + offset + descriptor.pcm_bytes,
            0,
            allocation_bytes - descriptor.pcm_bytes);
    }

    Entry& entry = entries_[entry_index];
    entry.occupied = true;
    entry.descriptor = descriptor;
    entry.offset = offset;
    entry.allocated_bytes = allocation_bytes;
    entry.pin_count = 0;
    entry.voice_ref_count = 0;
    entry.generation = nextGeneration();
    entry.touch_sequence = nextTouchSequence();

    used_bytes_ += allocation_bytes;
    if (used_bytes_ > high_water_bytes_) {
        high_water_bytes_ = used_bytes_;
    }
    setError(ResidencyError::None);
    return true;
}

bool SampleResidencyManager::isResident(SampleId sample_id) const {
    return initialized_ && findEntryIndex(sample_id) >= 0;
}

bool SampleResidencyManager::pin(SampleId sample_id) {
    if (!initialized_) {
        setError(ResidencyError::NotInitialized);
        return false;
    }
    const int index = findEntryIndex(sample_id);
    if (index < 0) {
        setError(ResidencyError::NotResident);
        return false;
    }
    Entry& entry = entries_[static_cast<std::size_t>(index)];
    if (entry.pin_count == std::numeric_limits<std::uint16_t>::max()) {
        setError(ResidencyError::InvalidArgument);
        return false;
    }
    ++entry.pin_count;
    entry.touch_sequence = nextTouchSequence();
    setError(ResidencyError::None);
    return true;
}

bool SampleResidencyManager::unpin(SampleId sample_id) {
    if (!initialized_) {
        setError(ResidencyError::NotInitialized);
        return false;
    }
    const int index = findEntryIndex(sample_id);
    if (index < 0) {
        setError(ResidencyError::NotResident);
        return false;
    }
    Entry& entry = entries_[static_cast<std::size_t>(index)];
    if (entry.pin_count == 0) {
        setError(ResidencyError::PinUnderflow);
        return false;
    }
    --entry.pin_count;
    entry.touch_sequence = nextTouchSequence();
    setError(ResidencyError::None);
    return true;
}

SampleRef SampleResidencyManager::acquire(SampleId sample_id) {
    SampleRef ref{};
    if (!initialized_) {
        setError(ResidencyError::NotInitialized);
        return ref;
    }

    const int entry_index_int = findEntryIndex(sample_id);
    if (entry_index_int < 0) {
        setError(ResidencyError::NotResident);
        return ref;
    }
    const int lease_index = allocateLease();
    if (lease_index < 0) {
        setError(ResidencyError::LeaseExhausted);
        return ref;
    }

    Entry& entry = entries_[static_cast<std::size_t>(entry_index_int)];
    if (entry.voice_ref_count == std::numeric_limits<std::uint16_t>::max()) {
        setError(ResidencyError::LeaseExhausted);
        return ref;
    }

    const std::uint32_t token = nextLeaseToken();
    Lease& lease = leases_[static_cast<std::size_t>(lease_index)];
    lease.active = true;
    lease.token = token;
    lease.sample_id = sample_id;
    lease.generation = entry.generation;
    lease.entry_index = static_cast<std::uint16_t>(entry_index_int);

    ++entry.voice_ref_count;
    entry.touch_sequence = nextTouchSequence();

    ref.sample_id = sample_id;
    ref.generation = entry.generation;
    ref.lease_token = token;
    ref.pcm = reinterpret_cast<const std::int16_t*>(arena_ + entry.offset);
    ref.descriptor = entry.descriptor;
    setError(ResidencyError::None);
    return ref;
}

bool SampleResidencyManager::release(const SampleRef& ref) {
    if (!initialized_) {
        setError(ResidencyError::NotInitialized);
        return false;
    }
    if (!ref.valid()) {
        ++stale_reference_faults_;
        setError(ResidencyError::StaleReference);
        return false;
    }

    Lease* matched = nullptr;
    for (Lease& lease : leases_) {
        if (lease.active && lease.token == ref.lease_token) {
            matched = &lease;
            break;
        }
    }
    if (matched == nullptr || matched->sample_id != ref.sample_id ||
        matched->generation != ref.generation ||
        matched->entry_index >= entries_.size()) {
        ++stale_reference_faults_;
        setError(ResidencyError::StaleReference);
        return false;
    }

    Entry& entry = entries_[matched->entry_index];
    if (!entry.occupied || entry.descriptor.id != ref.sample_id ||
        entry.generation != ref.generation || entry.voice_ref_count == 0) {
        ++stale_reference_faults_;
        setError(ResidencyError::StaleReference);
        return false;
    }

    --entry.voice_ref_count;
    entry.touch_sequence = nextTouchSequence();
    *matched = Lease{};
    setError(ResidencyError::None);
    return true;
}

bool SampleResidencyManager::evict(SampleId sample_id) {
    if (!initialized_) {
        setError(ResidencyError::NotInitialized);
        return false;
    }
    const int index = findEntryIndex(sample_id);
    if (index < 0) {
        setError(ResidencyError::NotResident);
        return false;
    }

    Entry& entry = entries_[static_cast<std::size_t>(index)];
    if (entry.pin_count != 0 || entry.voice_ref_count != 0) {
        setError(ResidencyError::Busy);
        return false;
    }

    if (!freeSpan(entry.offset, entry.allocated_bytes)) {
        setError(ResidencyError::MetadataFull);
        return false;
    }

    removeHash(sample_id);
    used_bytes_ -= entry.allocated_bytes;
    entry = Entry{};
    setError(ResidencyError::None);
    return true;
}

bool SampleResidencyManager::reset() {
    if (!initialized_) {
        setError(ResidencyError::NotInitialized);
        return false;
    }

    for (const Entry& entry : entries_) {
        if (entry.occupied && (entry.pin_count != 0 || entry.voice_ref_count != 0)) {
            setError(ResidencyError::Busy);
            return false;
        }
    }

    entries_.fill(Entry{});
    hash_.fill(HashSlot{});
    free_spans_.fill(Span{});
    leases_.fill(Lease{});
    free_span_count_ = 1;
    free_spans_[0] = Span{0, arena_bytes_};
    used_bytes_ = 0;
    setError(ResidencyError::None);
    return true;
}

ResidencyStats SampleResidencyManager::stats() const {
    ResidencyStats result{};
    result.capacity_bytes = arena_bytes_;
    result.used_bytes = used_bytes_;
    result.high_water_bytes = high_water_bytes_;
    result.free_span_count = free_span_count_;
    result.allocation_failures = allocation_failures_;
    result.stale_reference_faults = stale_reference_faults_;
    result.descriptor_conflicts = descriptor_conflicts_;

    for (std::size_t i = 0; i < free_span_count_; ++i) {
        if (free_spans_[i].bytes > result.largest_free_span_bytes) {
            result.largest_free_span_bytes = free_spans_[i].bytes;
        }
    }

    for (const Entry& entry : entries_) {
        if (!entry.occupied) {
            continue;
        }
        ++result.resident_sample_count;
        if (entry.pin_count != 0) {
            ++result.pinned_sample_count;
        }
        if (entry.voice_ref_count != 0) {
            ++result.referenced_sample_count;
        }
    }

    for (const Lease& lease : leases_) {
        if (lease.active) {
            ++result.active_lease_count;
        }
    }
    return result;
}

void SampleResidencyManager::clearState() {
    entries_.fill(Entry{});
    hash_.fill(HashSlot{});
    free_spans_.fill(Span{});
    leases_.fill(Lease{});
    free_span_count_ = 0;
    used_bytes_ = 0;
}

}  // namespace gm_synth::sample
