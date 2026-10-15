#pragma once

#include <cstddef>

namespace gm_synth::platform {

struct PsramHeapStats {
    std::size_t total_bytes = 0;
    std::size_t free_bytes = 0;
    std::size_t minimum_free_bytes = 0;
    std::size_t largest_free_block_bytes = 0;
};

class EspPsramArena {
public:
    EspPsramArena() = default;
    ~EspPsramArena();

    EspPsramArena(const EspPsramArena&) = delete;
    EspPsramArena& operator=(const EspPsramArena&) = delete;

    bool allocate(std::size_t bytes);
    void reset();

    void* data() const { return data_; }
    std::size_t capacity() const { return capacity_bytes_; }
    bool valid() const { return data_ != nullptr && capacity_bytes_ != 0; }

    static PsramHeapStats heapStats();

private:
    void* data_ = nullptr;
    std::size_t capacity_bytes_ = 0;
};

}  // namespace gm_synth::platform
