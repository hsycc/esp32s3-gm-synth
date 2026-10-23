#include "EspStorageBenchmark.h"

#include "esp_attr.h"
#include "esp_log.h"
#include "esp_timer.h"

#include <cinttypes>

namespace gm_synth::platform {
namespace {

constexpr char kTag[] = "storage_bench";
constexpr std::size_t kLatencyScratchCount = 2048;

class EspTimerClock final : public gm_synth::storage::BenchmarkClock {
public:
    std::uint64_t nowMicros() const override {
        const std::int64_t now = esp_timer_get_time();
        return now < 0 ? 0u : static_cast<std::uint64_t>(now);
    }
};

void logResult(
    std::size_t chunk_size,
    gm_synth::storage::StorageBenchmarkStatus status,
    const gm_synth::storage::StorageReadBenchmarkResult& result) {
    if (status != gm_synth::storage::StorageBenchmarkStatus::Ok) {
        ESP_LOGE(
            kTag,
            "chunk=%u status=%s bytes=%u/%u elapsed=%" PRIu64 " us",
            static_cast<unsigned>(chunk_size),
            gm_synth::storage::storageBenchmarkStatusString(status),
            static_cast<unsigned>(result.bytes_read),
            static_cast<unsigned>(result.requested_bytes),
            result.elapsed_us);
        return;
    }

    ESP_LOGI(
        kTag,
        "chunk=%u bytes=%u chunks=%u elapsed=%" PRIu64
        " us throughput=%.3f MiB/s latency_us[min/avg/p50/p95/p99/max]=%" PRIu64
        "/%" PRIu64 "/%" PRIu64 "/%" PRIu64 "/%" PRIu64 "/%" PRIu64,
        static_cast<unsigned>(chunk_size),
        static_cast<unsigned>(result.bytes_read),
        static_cast<unsigned>(result.chunk_count),
        result.elapsed_us,
        result.throughput_mib_per_second,
        result.min_chunk_us,
        result.average_chunk_us,
        result.p50_chunk_us,
        result.p95_chunk_us,
        result.p99_chunk_us,
        result.max_chunk_us);
}

}  // namespace

bool runEspStorageBenchmarkSweep(
    gm_synth::storage::RandomAccessReader& reader,
    void* psram_destination,
    std::size_t psram_capacity,
    std::uint64_t source_offset,
    std::size_t bytes_to_read,
    EspStorageBenchmarkSweepResult* sweep_result,
    EspStorageBenchmarkBetweenCaseHook between_case_hook,
    void* between_case_context) {
    if (sweep_result == nullptr) {
        return false;
    }

    // This scratch stores one 64-bit latency sample per storage chunk and is
    // not consumed by the realtime audio path. In the physical N16R8 image,
    // sdkconfig.hwbench enables external-BSS placement so this 16 KiB array
    // lives in PSRAM instead of exhausting scarce internal DRAM. Per-chunk
    // latency is captured before the scratch write, preserving latency timing.
    static EXT_RAM_BSS_ATTR std::uint64_t latency_scratch[kLatencyScratchCount];
    EspTimerClock clock;
    bool all_ok = true;

    ESP_LOGI(
        kTag,
        "starting VFS->PSRAM sweep: offset=%" PRIu64 " bytes=%u destination_capacity=%u",
        source_offset,
        static_cast<unsigned>(bytes_to_read),
        static_cast<unsigned>(psram_capacity));

    for (std::size_t i = 0; i < kStandardStorageBenchmarkChunkSizes.size(); ++i) {
        gm_synth::storage::StorageReadBenchmarkConfig config{};
        config.source_offset = source_offset;
        config.bytes_to_read = bytes_to_read;
        config.chunk_size = kStandardStorageBenchmarkChunkSizes[i];

        auto& result = sweep_result->results[i];
        const auto status = gm_synth::storage::runStorageReadBenchmark(
            reader,
            psram_destination,
            psram_capacity,
            config,
            clock,
            latency_scratch,
            kLatencyScratchCount,
            &result);

        sweep_result->statuses[i] = status;
        logResult(config.chunk_size, status, result);
        all_ok = all_ok && status == gm_synth::storage::StorageBenchmarkStatus::Ok;

        if (between_case_hook != nullptr &&
            !between_case_hook(config.chunk_size, between_case_context)) {
            ESP_LOGE(kTag, "between-case hook failed after chunk=%u",
                     static_cast<unsigned>(config.chunk_size));
            return false;
        }
    }

    return all_ok;
}

}  // namespace gm_synth::platform
