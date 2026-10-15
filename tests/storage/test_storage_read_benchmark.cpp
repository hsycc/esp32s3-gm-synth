#include "../../firmware/storage/StorageReadBenchmark.h"

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>

using gm_synth::storage::BenchmarkClock;
using gm_synth::storage::RandomAccessReader;
using gm_synth::storage::StorageBenchmarkStatus;
using gm_synth::storage::StorageReadBenchmarkConfig;
using gm_synth::storage::StorageReadBenchmarkResult;
using gm_synth::storage::runStorageReadBenchmark;

namespace {

int failures = 0;

#define CHECK(expr)                                                                    \
    do {                                                                               \
        if (!(expr)) {                                                                 \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #expr);     \
            ++failures;                                                                \
        }                                                                              \
    } while (0)

class MemoryReader final : public RandomAccessReader {
public:
    explicit MemoryReader(std::size_t fail_on_call = static_cast<std::size_t>(-1))
        : fail_on_call_(fail_on_call) {
        for (std::size_t i = 0; i < bytes_.size(); ++i) {
            bytes_[i] = static_cast<std::uint8_t>((i * 19u + 7u) & 0xffu);
        }
    }

    std::uint64_t size() const override { return bytes_.size(); }

    bool readAt(std::uint64_t offset, void* destination, std::size_t bytes) override {
        const std::size_t call = call_count_++;
        if (call == fail_on_call_) {
            return false;
        }
        if (destination == nullptr || offset > bytes_.size() || bytes > bytes_.size() - offset) {
            return false;
        }

        auto* out = static_cast<std::uint8_t*>(destination);
        for (std::size_t i = 0; i < bytes; ++i) {
            out[i] = bytes_[static_cast<std::size_t>(offset) + i];
        }
        return true;
    }

    std::uint8_t expected(std::size_t index) const { return bytes_[index]; }

private:
    std::array<std::uint8_t, 4096> bytes_{};
    std::size_t fail_on_call_;
    std::size_t call_count_ = 0;
};

class ScriptClock final : public BenchmarkClock {
public:
    ScriptClock(const std::uint64_t* values, std::size_t count)
        : values_(values), count_(count) {}

    std::uint64_t nowMicros() const override {
        if (index_ >= count_) {
            return count_ == 0 ? 0 : values_[count_ - 1u];
        }
        return values_[index_++];
    }

private:
    const std::uint64_t* values_;
    std::size_t count_;
    mutable std::size_t index_ = 0;
};

void test_success_and_exact_percentiles() {
    MemoryReader reader;
    std::array<std::uint8_t, 4096> destination{};
    std::array<std::uint64_t, 4> latencies{};
    constexpr std::array<std::uint64_t, 10> times{
        1000, 1000, 1010, 1010, 1030, 1030, 1060, 1060, 1100, 1100};
    ScriptClock clock(times.data(), times.size());

    StorageReadBenchmarkConfig config{};
    config.source_offset = 0;
    config.bytes_to_read = destination.size();
    config.chunk_size = 1024;

    StorageReadBenchmarkResult result{};
    const auto status = runStorageReadBenchmark(
        reader,
        destination.data(),
        destination.size(),
        config,
        clock,
        latencies.data(),
        latencies.size(),
        &result);

    CHECK(status == StorageBenchmarkStatus::Ok);
    CHECK(result.status == StorageBenchmarkStatus::Ok);
    CHECK(result.bytes_read == 4096);
    CHECK(result.chunk_count == 4);
    CHECK(result.elapsed_us == 100);
    CHECK(result.min_chunk_us == 10);
    CHECK(result.average_chunk_us == 25);
    CHECK(result.p50_chunk_us == 20);
    CHECK(result.p95_chunk_us == 40);
    CHECK(result.p99_chunk_us == 40);
    CHECK(result.max_chunk_us == 40);
    CHECK(std::fabs(result.throughput_mib_per_second - 39.0625) < 0.0001);

    for (std::size_t i = 0; i < destination.size(); ++i) {
        CHECK(destination[i] == reader.expected(i));
    }
}

void test_read_failure_preserves_partial_stats() {
    MemoryReader reader(1);
    std::array<std::uint8_t, 4096> destination{};
    std::array<std::uint64_t, 4> latencies{};
    constexpr std::array<std::uint64_t, 5> times{1000, 1000, 1010, 1010, 1035};
    ScriptClock clock(times.data(), times.size());

    StorageReadBenchmarkConfig config{};
    config.bytes_to_read = destination.size();
    config.chunk_size = 1024;

    StorageReadBenchmarkResult result{};
    const auto status = runStorageReadBenchmark(
        reader,
        destination.data(),
        destination.size(),
        config,
        clock,
        latencies.data(),
        latencies.size(),
        &result);

    CHECK(status == StorageBenchmarkStatus::ReadFailed);
    CHECK(result.bytes_read == 1024);
    CHECK(result.chunk_count == 1);
    CHECK(result.elapsed_us == 35);
    CHECK(result.min_chunk_us == 10);
    CHECK(result.max_chunk_us == 10);
}

void test_rejects_bad_ranges_and_scratch_capacity() {
    MemoryReader reader;
    std::array<std::uint8_t, 4096> destination{};
    std::array<std::uint64_t, 3> too_small{};
    constexpr std::array<std::uint64_t, 1> times{0};
    ScriptClock clock(times.data(), times.size());

    StorageReadBenchmarkConfig config{};
    config.source_offset = 0;
    config.bytes_to_read = 4096;
    config.chunk_size = 1024;

    StorageReadBenchmarkResult result{};
    CHECK(runStorageReadBenchmark(
              reader,
              destination.data(),
              destination.size(),
              config,
              clock,
              too_small.data(),
              too_small.size(),
              &result) == StorageBenchmarkStatus::LatencyStorageTooSmall);

    std::array<std::uint64_t, 4> scratch{};
    config.source_offset = 1;
    CHECK(runStorageReadBenchmark(
              reader,
              destination.data(),
              destination.size(),
              config,
              clock,
              scratch.data(),
              scratch.size(),
              &result) == StorageBenchmarkStatus::SourceRangeError);

    config.source_offset = 0;
    CHECK(runStorageReadBenchmark(
              reader,
              destination.data(),
              destination.size() - 1u,
              config,
              clock,
              scratch.data(),
              scratch.size(),
              &result) == StorageBenchmarkStatus::DestinationTooSmall);
}

}  // namespace

int main() {
    test_success_and_exact_percentiles();
    test_read_failure_preserves_partial_stats();
    test_rejects_bad_ranges_and_scratch_capacity();

    if (failures != 0) {
        std::fprintf(stderr, "%d storage benchmark checks failed\n", failures);
        return 1;
    }

    std::printf("Storage read benchmark host checks passed\n");
    return 0;
}
