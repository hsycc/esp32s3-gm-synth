#pragma once

#include "storage/StorageReadBenchmark.h"

#include <array>
#include <cstddef>
#include <cstdint>

namespace gm_synth::platform {

inline constexpr std::array<std::size_t, 5> kStandardStorageBenchmarkChunkSizes{
    4u * 1024u,
    8u * 1024u,
    16u * 1024u,
    32u * 1024u,
    64u * 1024u,
};

struct EspStorageBenchmarkSweepResult {
    std::array<gm_synth::storage::StorageBenchmarkStatus, kStandardStorageBenchmarkChunkSizes.size()>
        statuses{};
    std::array<gm_synth::storage::StorageReadBenchmarkResult,
               kStandardStorageBenchmarkChunkSizes.size()>
        results{};
};

// Runs the standard 4/8/16/32/64 KiB sweep into caller-owned PSRAM.
// This function is intended for bring-up/control tasks only, never the audio task.
bool runEspStorageBenchmarkSweep(
    gm_synth::storage::RandomAccessReader& reader,
    void* psram_destination,
    std::size_t psram_capacity,
    std::uint64_t source_offset,
    std::size_t bytes_to_read,
    EspStorageBenchmarkSweepResult* sweep_result);

}  // namespace gm_synth::platform
