#include "SampleResidencyManager.h"

#include <cstring>

namespace gm_synth::sample {

bool SampleResidencyManager::loadFromSource(
    const SampleDescriptor& descriptor,
    SampleFillCallback fill,
    void* context) {
    if (!initialized_) {
        setError(ResidencyError::NotInitialized);
        return false;
    }
    if (fill == nullptr || descriptor.pcm_bytes == 0 ||
        (descriptor.pcm_bytes & 1u) != 0u) {
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

    // The destination is not present in hash_/entries_ yet. Only this
    // synchronous control/storage callback can see the reserved span.
    if (!fill(context, arena_ + offset, descriptor.pcm_bytes)) {
        (void)freeSpan(offset, allocation_bytes);
        setError(ResidencyError::SourceFillFailed);
        return false;
    }

    if (allocation_bytes > descriptor.pcm_bytes) {
        std::memset(
            arena_ + offset + descriptor.pcm_bytes,
            0,
            allocation_bytes - descriptor.pcm_bytes);
    }

    const std::uint16_t entry_index = static_cast<std::uint16_t>(free_entry_index);
    if (!insertHash(descriptor.id, entry_index)) {
        (void)freeSpan(offset, allocation_bytes);
        ++allocation_failures_;
        setError(ResidencyError::MetadataFull);
        return false;
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

}  // namespace gm_synth::sample
