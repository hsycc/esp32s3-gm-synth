#pragma once

#include "SampleResidencyManager.h"

#include <cstddef>
#include <cstdint>

namespace gm_synth::sample {

// Control/storage-domain source for one SoundFont bank.
//
// Implementations may read from Flash, LittleFS, SD/MMC or another random-
// access backend. These methods are never called from the audio task.
class SampleSource {
public:
    virtual ~SampleSource() = default;

    // Return the runtime descriptor for one bank-local SampleId. The descriptor
    // must contain the same SampleId and an even, non-zero 16-bit PCM byte size.
    virtual bool descriptorFor(SampleId sample_id, SampleDescriptor* descriptor) = 0;

    // Fill exactly pcm_bytes bytes directly into the destination supplied by
    // SampleResidencyManager::loadFromSource(). Implementations should perform
    // bounded/chunked storage reads instead of allocating a sample-sized
    // staging buffer.
    virtual bool fillSample(
        SampleId sample_id,
        std::uint8_t* destination,
        std::size_t pcm_bytes) = 0;
};

}  // namespace gm_synth::sample
