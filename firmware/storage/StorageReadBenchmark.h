#pragma once

#include "RandomAccessReader.h"

#include <cstddef>
#include <cstdint>

namespace gm_synth::storage {

enum class StorageBenchmarkStatus : std::uint8_t {
    Ok = 0,
    InvalidArgument,
    SourceRangeError,
    DestinationTooSmall,
    LatencyStorageTooSmall,
    ClockError,
    ReadFailed,
};

const char* storageBenchmarkStatusString(StorageBenchmarkStatus status);

class BenchmarkClock {
public:
    virtual ~BenchmarkClock() = default;
    virtual std::uint64_t nowMicros() const = 0;
};

struct StorageReadBenchmarkConfig {
    std::uint64_t source_offset = 0;
    std::size_t bytes_to_read = 0;
    std::size_t chunk_size = 0;
};

struct StorageReadBenchmarkResult {
    StorageBenchmarkStatus status = StorageBenchmarkStatus::InvalidArgument;
    std::size_t requested_bytes = 0;
    std::size_t bytes_read = 0;
    std::size_t chunk_size = 0;
    std::size_t chunk_count = 0;
    std::uint64_t elapsed_us = 0;
    std::uint64_t min_chunk_us = 0;
    std::uint64_t average_chunk_us = 0;
    std::uint64_t p50_chunk_us = 0;
    std::uint64_t p95_chunk_us = 0;
    std::uint64_t p99_chunk_us = 0;
    std::uint64_t max_chunk_us = 0;
    double throughput_mib_per_second = 0.0;
};

// Measures storage -> destination throughput while writing every chunk directly to
// its final destination address. The caller owns both destination memory and the
// latency scratch buffer so this function never allocates and is suitable for a
// Control/Storage task on ESP32-S3.
//
// latency_scratch is reordered while calculating exact nearest-rank percentiles.
StorageBenchmarkStatus runStorageReadBenchmark(
    RandomAccessReader& reader,
    void* destination,
    std::size_t destination_capacity,
    const StorageReadBenchmarkConfig& config,
    BenchmarkClock& clock,
    std::uint64_t* latency_scratch,
    std::size_t latency_capacity,
    StorageReadBenchmarkResult* result);

}  // namespace gm_synth::storage
