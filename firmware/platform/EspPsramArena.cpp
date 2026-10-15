#include "EspPsramArena.h"

#include "esp_heap_caps.h"

#include <cstdint>

namespace gm_synth::platform {
namespace {

constexpr std::uint32_t kPsramCaps = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;
constexpr std::size_t kArenaAlignment = 4;

}  // namespace

EspPsramArena::~EspPsramArena() {
    reset();
}

bool EspPsramArena::allocate(std::size_t bytes) {
    reset();
    if (bytes == 0) {
        return false;
    }

    data_ = heap_caps_aligned_alloc(kArenaAlignment, bytes, kPsramCaps);
    if (data_ == nullptr) {
        return false;
    }

    capacity_bytes_ = bytes;
    return true;
}

void EspPsramArena::reset() {
    if (data_ != nullptr) {
        heap_caps_free(data_);
        data_ = nullptr;
    }
    capacity_bytes_ = 0;
}

PsramHeapStats EspPsramArena::heapStats() {
    PsramHeapStats stats{};
    stats.total_bytes = heap_caps_get_total_size(kPsramCaps);
    stats.free_bytes = heap_caps_get_free_size(kPsramCaps);
    stats.minimum_free_bytes = heap_caps_get_minimum_free_size(kPsramCaps);
    stats.largest_free_block_bytes = heap_caps_get_largest_free_block(kPsramCaps);
    return stats;
}

}  // namespace gm_synth::platform
