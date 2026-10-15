#include "StorageReadBenchmark.h"

#include <algorithm>
#include <cstdint>

namespace gm_synth::storage {
namespace {

std::size_t nearestRankIndex(std::size_t count, std::size_t percentile) {
    const std::size_t rank = (percentile * count + 99u) / 100u;
    return rank == 0 ? 0 : rank - 1u;
}

void finalizeResult(
    StorageReadBenchmarkResult* result,
    std::uint64_t* latency_scratch,
    std::size_t latency_count) {
    if (result == nullptr) {
        return;
    }

    if (result->elapsed_us != 0u && result->bytes_read != 0u) {
        constexpr double kBytesPerMiB = 1024.0 * 1024.0;
        constexpr double kMicrosPerSecond = 1000000.0;
        result->throughput_mib_per_second =
            (static_cast<double>(result->bytes_read) * kMicrosPerSecond) /
            (static_cast<double>(result->elapsed_us) * kBytesPerMiB);
    }

    if (latency_count == 0u || latency_scratch == nullptr) {
        return;
    }

    std::sort(latency_scratch, latency_scratch + latency_count);

    std::uint64_t sum = 0;
    for (std::size_t i = 0; i < latency_count; ++i) {
        sum += latency_scratch[i];
    }

    result->min_chunk_us = latency_scratch[0];
    result->average_chunk_us = sum / latency_count;
    result->p50_chunk_us = latency_scratch[nearestRankIndex(latency_count, 50u)];
    result->p95_chunk_us = latency_scratch[nearestRankIndex(latency_count, 95u)];
    result->p99_chunk_us = latency_scratch[nearestRankIndex(latency_count, 99u)];
    result->max_chunk_us = latency_scratch[latency_count - 1u];
}

}  // namespace

const char* storageBenchmarkStatusString(StorageBenchmarkStatus status) {
    switch (status) {
        case StorageBenchmarkStatus::Ok:
            return "ok";
        case StorageBenchmarkStatus::InvalidArgument:
            return "invalid_argument";
        case StorageBenchmarkStatus::SourceRangeError:
            return "source_range_error";
        case StorageBenchmarkStatus::DestinationTooSmall:
            return "destination_too_small";
        case StorageBenchmarkStatus::LatencyStorageTooSmall:
            return "latency_storage_too_small";
        case StorageBenchmarkStatus::ClockError:
            return "clock_error";
        case StorageBenchmarkStatus::ReadFailed:
            return "read_failed";
    }
    return "unknown";
}

StorageBenchmarkStatus runStorageReadBenchmark(
    RandomAccessReader& reader,
    void* destination,
    std::size_t destination_capacity,
    const StorageReadBenchmarkConfig& config,
    BenchmarkClock& clock,
    std::uint64_t* latency_scratch,
    std::size_t latency_capacity,
    StorageReadBenchmarkResult* result) {
    if (result == nullptr) {
        return StorageBenchmarkStatus::InvalidArgument;
    }

    *result = {};
    result->requested_bytes = config.bytes_to_read;
    result->chunk_size = config.chunk_size;

    if (destination == nullptr || config.bytes_to_read == 0u || config.chunk_size == 0u) {
        result->status = StorageBenchmarkStatus::InvalidArgument;
        return result->status;
    }

    if (config.bytes_to_read > destination_capacity) {
        result->status = StorageBenchmarkStatus::DestinationTooSmall;
        return result->status;
    }

    const std::uint64_t source_size = reader.size();
    if (config.source_offset > source_size ||
        static_cast<std::uint64_t>(config.bytes_to_read) > source_size - config.source_offset) {
        result->status = StorageBenchmarkStatus::SourceRangeError;
        return result->status;
    }

    const std::size_t required_chunks =
        (config.bytes_to_read + config.chunk_size - 1u) / config.chunk_size;
    if (latency_scratch == nullptr || latency_capacity < required_chunks) {
        result->status = StorageBenchmarkStatus::LatencyStorageTooSmall;
        return result->status;
    }

    auto* output = static_cast<std::uint8_t*>(destination);
    const std::uint64_t overall_start = clock.nowMicros();
    std::size_t completed_chunks = 0;

    while (result->bytes_read < config.bytes_to_read) {
        const std::size_t remaining = config.bytes_to_read - result->bytes_read;
        const std::size_t this_chunk = std::min(config.chunk_size, remaining);
        const std::uint64_t chunk_start = clock.nowMicros();

        const bool read_ok = reader.readAt(
            config.source_offset + static_cast<std::uint64_t>(result->bytes_read),
            output + result->bytes_read,
            this_chunk);

        const std::uint64_t chunk_end = clock.nowMicros();
        if (chunk_end < chunk_start || chunk_end < overall_start) {
            result->status = StorageBenchmarkStatus::ClockError;
            result->elapsed_us = chunk_end >= overall_start ? chunk_end - overall_start : 0u;
            result->chunk_count = completed_chunks;
            finalizeResult(result, latency_scratch, completed_chunks);
            return result->status;
        }

        if (!read_ok) {
            result->status = StorageBenchmarkStatus::ReadFailed;
            result->elapsed_us = chunk_end - overall_start;
            result->chunk_count = completed_chunks;
            finalizeResult(result, latency_scratch, completed_chunks);
            return result->status;
        }

        latency_scratch[completed_chunks] = chunk_end - chunk_start;
        ++completed_chunks;
        result->bytes_read += this_chunk;
    }

    const std::uint64_t overall_end = clock.nowMicros();
    if (overall_end < overall_start) {
        result->status = StorageBenchmarkStatus::ClockError;
        result->chunk_count = completed_chunks;
        finalizeResult(result, latency_scratch, completed_chunks);
        return result->status;
    }

    result->elapsed_us = overall_end - overall_start;
    result->chunk_count = completed_chunks;
    result->status = StorageBenchmarkStatus::Ok;
    finalizeResult(result, latency_scratch, completed_chunks);
    return result->status;
}

}  // namespace gm_synth::storage
