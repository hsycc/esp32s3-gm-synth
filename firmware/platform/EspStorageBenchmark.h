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

using EspStorageBenchmarkBetweenCaseHook = bool (*)(std::size_t chunk_size, void* context);

// Runs the standard 4/8/16/32/64 KiB sweep into caller-owned PSRAM.
// The optional between-case hook runs only after a measured case has finished,
// so qualification scheduling work cannot contaminate the case timing itself.
// This function is intended for bring-up/control tasks only, never the audio task.
bool runEspStorageBenchmarkSweep(
    gm_synth::storage::RandomAccessReader& reader,
    void* psram_destination,
    std::size_t psram_capacity,
    std::uint64_t source_offset,
    std::size_t bytes_to_read,
    EspStorageBenchmarkSweepResult* sweep_result,
    EspStorageBenchmarkBetweenCaseHook between_case_hook = nullptr,
    void* between_case_context = nullptr);

}  // namespace gm_synth::platform
