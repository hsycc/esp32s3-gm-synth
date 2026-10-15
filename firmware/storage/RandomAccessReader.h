#pragma once

#include <cstddef>
#include <cstdint>

namespace gm_synth::storage {

class RandomAccessReader {
public:
    virtual ~RandomAccessReader() = default;

    virtual std::uint64_t size() const = 0;
    virtual bool readAt(std::uint64_t offset, void* destination, std::size_t bytes) = 0;
};

}  // namespace gm_synth::storage
